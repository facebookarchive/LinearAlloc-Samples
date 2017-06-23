/**
 * Copyright (c) 2012-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant 
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <jni.h>
#include <android/log.h>


#define LOG_TAG "dalvik-internals"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


// Cached JavaVM pointer from JNI_OnLoad
static JavaVM* loadedJavaVM;

// Pre-find some exception classes to make error reporting easier.
static jclass clsIllegalStateException;
static jclass clsNullPointerException;
static jclass clsIOException;


// Pointers to some important symbols in libdvm.  Weak symbols don't work here,
// so we find these with dlsym.
static jint (*p_JNI_GetCreatedJavaVMs)(JavaVM**, jsize, jsize*);

static void (*p_ladDumpProfiles)(
    const char* stackProfileFilename,
    const char* classProfileFilename);

static void (*p_ladResetProfiles)();

static void (*p_ladPrintHeaderInfo)();

static int initted = false;
static int init(JNIEnv* env);

static uint8_t* p_gDvm;


// Utility functions.

/**
 * Throws a Java IOException with the given message, which must be under 100 bytes.
 * Includes errno in the error message.
 */
void throwIoException(JNIEnv* env, const char* message) {
  int saved_errno = errno;
  char err_msg[128];
  size_t messageLength = strlen(message);
  (void)messageLength;
  assert(messageLength < 100);
  snprintf(err_msg, sizeof(err_msg), "%s (errno = %d)", message, saved_errno);
  (*env)->ThrowNew(env, clsIOException, err_msg);
}


// Smoke-testing function.
jstring Java_com_facebook_dalvik_DalvikInternals_getTestString(
    JNIEnv* env, jobject clazz) {
  (void)clazz;
  if (!initted && !init(env)) {
    return NULL;
  }

  return (*env)->NewStringUTF(env, "Message from JNI.");
}


// Simple JNI wrapper for ladDumpProfiles.
void Java_com_facebook_dalvik_DalvikInternals_dumpLinearAllocProfiles(
    JNIEnv* env, jobject clazz,
    jstring stackProfileFileString, jstring classProfileFileString) {
  (void)clazz;

  if (!initted && !init(env)) {
    return;
  }

  if (p_ladDumpProfiles == NULL) {
    (*env)->ThrowNew(env, clsIllegalStateException,
        "ladDumpProfiles not available on this system.");
    return;
  }

  if (stackProfileFileString == NULL) {
    (*env)->ThrowNew(env, clsNullPointerException,
        "stackProfileFileString");
    return;
  }
  if (classProfileFileString == NULL) {
    (*env)->ThrowNew(env, clsNullPointerException,
        "classProfileFileString");
    return;
  }

  const char* stackProfileFilename =
    (*env)->GetStringUTFChars(env, stackProfileFileString, NULL);
  if (stackProfileFilename == NULL) {
    return;
  }
  const char* classProfileFilename =
    (*env)->GetStringUTFChars(env, classProfileFileString, NULL);
  if (classProfileFilename == NULL) {
    (*env)->ReleaseStringUTFChars(env,
        stackProfileFileString, stackProfileFilename);
    return;
  }

  p_ladDumpProfiles(stackProfileFilename, classProfileFilename);

  (*env)->ReleaseStringUTFChars(env,
      stackProfileFileString, stackProfileFilename);
  (*env)->ReleaseStringUTFChars(env,
      classProfileFileString, classProfileFilename);
}


// Simple JNI wrapper for ladResetProfiles.
void Java_com_facebook_dalvik_DalvikInternals_resetLinearAllocProfiles(
    JNIEnv* env, jobject clazz) {
  (void)clazz;

  if (!initted && !init(env)) {
    return;
  }

  if (p_ladResetProfiles == NULL) {
    (*env)->ThrowNew(env, clsIllegalStateException,
        "ladResetProfiles not available on this system.");
    return;
  }

  p_ladResetProfiles();
}


// Simple JNI wrapper for ladPrintHeaderInfo.
void Java_com_facebook_dalvik_DalvikInternals_printLinearAllocHeaderInfo(
    JNIEnv* env, jobject clazz) {
  (void)clazz;

  if (!initted && !init(env)) {
    return;
  }

  if (p_ladPrintHeaderInfo == NULL) {
    (*env)->ThrowNew(env, clsIllegalStateException,
        "ladPrintHeaderInfo not available on this system.");
    return;
  }

  p_ladPrintHeaderInfo();
}


// Structures and functions for querying our memory map to prevent segfaults.

/// Single memory mapping.  This is small enough to be passed around as a value.
typedef struct {
  /// Base address of the mapping.
  void* base;
  /// One-past the last valid address in the mapping.
  void* bound;
} MemoryMapEntry;

/// Full memory map.  Can also be passed by value.
typedef struct {
  /// List of all entries.
  MemoryMapEntry* entries;
  /// Number of entries.
  size_t size;
} MemoryMap;

/**
 * Convert a "flat memory map" from Java (from ProcSelfMaps) into a MemoryMap
 * structure, which we can query easily.
 *
 * On failure, the returned map will have a null pointer for entries and
 * a Java exception will be thrown.
 *
 * On success, entries is malloc()-allocated and must be free()ed.
 */
static MemoryMap convertFlatMemoryMap(JNIEnv* env, jlongArray flatMemoryMap) {
  MemoryMap map = { .entries = NULL };

  jsize longSize = (*env)->GetArrayLength(env, flatMemoryMap);
  size_t structSize = longSize / 2;

  MemoryMapEntry* ptrEntries = malloc(structSize * sizeof(*ptrEntries));
  if (ptrEntries == NULL) {
    throwIoException(env, "Unable to allocate memory for native memory map.");
    return map;
  }

  jlong* longEntries = (*env)->GetLongArrayElements(env, flatMemoryMap, NULL);
  if (longEntries == NULL) {
    // Exception already thrown.
    free(ptrEntries);
    return map;
  }

  map.entries = ptrEntries;
  map.size = structSize;

  for (size_t i = 0; i < structSize; i++) {
    assert(longEntries[i*2] < longEntries[i*2+1]);
    if (i != 0) {
      assert(longEntries[i*2-1] <= longEntries[i*2]);
    }
    map.entries[i].base = (void*)(uintptr_t)longEntries[i*2];
    map.entries[i].bound = (void*)(uintptr_t)longEntries[i*2+1];
  }

  (*env)->ReleaseLongArrayElements(env, flatMemoryMap, longEntries, 0);

  return map;
}

/**
 * Find the MemoryMapEntry that contains a given pointer, or NULL if none exists.
 */
static MemoryMapEntry* mapPointer(MemoryMap map, void* ptr) {
  int lo = 0;
  int hi = map.size - 1;

  while (lo <= hi) {
    int mid = lo + (hi - lo)/2;
    MemoryMapEntry* entry = &map.entries[mid];
    if (ptr < entry->base) {
      hi = mid - 1;
    } else if (ptr >= entry->bound) {
      lo = mid + 1;
    } else {
      return entry;
    }
  }

  return NULL;
}

/**
 * Returns the number of bytes that can be safely read, starting from ptr.
 */
static ptrdiff_t safeRangeFrom(MemoryMap map, void* ptr) {
  MemoryMapEntry* entry = mapPointer(map, ptr);
  if (entry == NULL) {
    return 0;
  }
  return entry->bound - ptr;
}


// Structures and functions for searching for the LinearAllocHdr.

// Just make these values a bit easier to pass around.
typedef struct {
  MemoryMap memoryMap;
  int minAllocated;
  int maxAllocated;
  int minBufferSize;
  int maxBufferSize;
  int firstOffset;
  void* mapAddr;
} LinearAllocCheckArgs;

// From dalvik/vm/LinearAlloc.h
typedef struct LinearAllocHdr {
    int     curOffset;          /* offset where next data goes */
    pthread_mutex_t lock;       /* controls updates to this struct */

    char*   mapAddr;            /* start of mmap()ed region */
    int     mapLength;          /* length of region */
    int     firstOffset;        /* for chasing through */

    short*  writeRefCount;      /* for ENFORCE_READ_ONLY */
} LinearAllocHdr;

enum CheckVerbosity {
  CHECK_SILENT = 0,
  CHECK_VERBOSE = 1,
};

jlong Java_com_facebook_dalvik_DalvikInternals_nativeFindLinearAllocHeader(
    JNIEnv* env, jobject clazz,
    jlongArray flatMemoryMap,
    jint javavmOffsetMin,
    jint javavmOffsetLimit,
    jint javavmToLinearAllocOffset,
    jint javavmToLinearAllocExtraChecks,
    jint minAllocated,
    jint maxAllocated,
    jlong heapStart,
    jlong heapEnd,
    jint minBufferSize,
    jint maxBufferSize,
    jint firstOffset,
    jlong mapAddr);

static void* findLinearAllocHeader(
    MemoryMap memoryMap,
    int javavmOffsetMin,
    int javavmOffsetLimit,
    int javavmToLinearAllocOffset,
    int javavmToLinearAllocExtraChecks,
    int minAllocated,
    int maxAllocated,
    void* heapStart,
    void* heapEnd,
    int minBufferSize,
    int maxBufferSize,
    int firstOffset,
    void* mapAddr);

static bool checkLinearAlloc(
    LinearAllocHdr* linearAlloc,
    LinearAllocCheckArgs checkArgs,
    enum CheckVerbosity verbose);

/**
 * Attempt to find the LinearAllocHdr used by Dalvik for class loading.
 * This is a wrapper that does some JNI bookkeeping, resource management, and sanity checking.
 */
jlong Java_com_facebook_dalvik_DalvikInternals_nativeFindLinearAllocHeader(
    JNIEnv* env, jobject clazz,
    jlongArray flatMemoryMap,
    jint javavmOffsetMin,
    jint javavmOffsetLimit,
    jint javavmToLinearAllocOffset,
    jint javavmToLinearAllocExtraChecks,
    jint minAllocated,
    jint maxAllocated,
    jlong heapStart,
    jlong heapEnd,
    jint minBufferSize,
    jint maxBufferSize,
    jint firstOffset,
    jlong mapAddr) {
  (void)clazz;

  if (!initted && !init(env))
  {
    return 0;
  }

  // Compare the JavaVM pointer passed to JNI_OnLoad to the one returned
  // from JNI_GetCreatedJavaVMs (which should be the one in gDvm).
  // This serves as a sanity check that we don't have two copies of
  // libdvm loaded.
  if (p_JNI_GetCreatedJavaVMs != NULL) {
    JavaVM* createdJavaVM;
    jsize totalJavaVMs;
    jint ret = p_JNI_GetCreatedJavaVMs(&createdJavaVM, 1, &totalJavaVMs);
    if (ret != 0) {
      LOGE("JNI_GetCreatedJavaVMs failed.");
    } else if (totalJavaVMs != 1) {
      LOGE("JNI_GetCreatedJavaVMs returned %d JavaVMs (expected 1).", totalJavaVMs);
    } else if (createdJavaVM != loadedJavaVM) {
      LOGE("JNI_GetCreatedJavaVMs returned a different JavaVM (%p, expected %p).",
          createdJavaVM, loadedJavaVM);
    }
  } else {
    LOGE("Could not find JNI_GetCreatedJavaVMs.  Skipping sanity check.");
  }
  // Ignore errors and press on.

  JavaVM* gottenJavaVM;
  jint ret = (*env)->GetJavaVM(env, &gottenJavaVM);
  if (ret != 0) {
    LOGE("GetJavaVM failed.");
    // Prints exception to stderr and clears it.
    (*env)->ExceptionDescribe(env);
  } else if (gottenJavaVM != loadedJavaVM) {
    LOGE("GetJavaVM returned a different JavaVM (%p, expected %p).",
        gottenJavaVM, loadedJavaVM);
  }
  // Ignore errors and press on.

  MemoryMap memoryMap = convertFlatMemoryMap(env, flatMemoryMap);
  if (memoryMap.entries == NULL) {
    return 0;
  }

  void* header = findLinearAllocHeader(
      memoryMap,
      javavmOffsetMin,
      javavmOffsetLimit,
      javavmToLinearAllocOffset,
      javavmToLinearAllocExtraChecks,
      minAllocated,
      maxAllocated,
      (void*)(uintptr_t)heapStart,
      (void*)(uintptr_t)heapEnd,
      minBufferSize,
      maxBufferSize,
      firstOffset,
      (void*)(uintptr_t)mapAddr);

  free(memoryMap.entries);

  return (jlong)(uintptr_t)header;
}

/**
 * Attempt to find the LinearAllocHdr used by Dalvik for class loading.
 * This function performs most of the logic and doesn't interact with Java.
 */
static void* findLinearAllocHeader(
    MemoryMap memoryMap,
    int javavmOffsetMin,
    int javavmOffsetLimit,
    int javavmToLinearAllocOffset,
    int javavmToLinearAllocExtraChecks,
    int minAllocated,
    int maxAllocated,
    void* heapStart,
    void* heapEnd,
    int minBufferSize,
    int maxBufferSize,
    int firstOffset,
    void* mapAddr) {
  LinearAllocCheckArgs checkArgs = {
    memoryMap,
    minAllocated,
    maxAllocated,
    minBufferSize,
    maxBufferSize,
    firstOffset,
    mapAddr,
  };

  // Our best bet is trying to find a pointer within gDvm.
  if (p_gDvm != NULL) {
    LOGI("gDvm has value %p.  Searching for vmList (initial offset in [%d,%d]).",
        p_gDvm, javavmOffsetMin, javavmOffsetLimit);
    // Shorten our limit if necessary to avoid segfaulting.
    int offsetLimit = javavmOffsetLimit;
    int memoryMapLimit = safeRangeFrom(memoryMap, p_gDvm);
    if (memoryMapLimit < offsetLimit) {
      LOGW("Premature end of safe memory.  Only searching %d bytes.", memoryMapLimit);
      offsetLimit = memoryMapLimit;
    }
    // Shorten our limit further, because once we find the JavaVM, we offset an additional
    // javavmToLinearAllocOffset bytes and (up to) javavmToLinearAllocExtraChecks words
    // before dereferencing.
    offsetLimit -= javavmToLinearAllocOffset + javavmToLinearAllocExtraChecks * sizeof(void*);

    // Attempt number 1: The pointer to the LinearAllocHdr is buried
    // over 700 bytes within gDvm.  However, it's only 24 bytes (on Gingerbread)
    // beyond vmList.  Fortunately, we know the value of vmList from JNI_OnLoad.
    // Scan gDvm looking for vmList, then offset from there.
    LOGI("Beginning search.  Actual offset in [%d,%d].", javavmOffsetMin, offsetLimit);
    for (int offset = javavmOffsetMin;
        offset + sizeof(void*) <= (size_t)offsetLimit;
        offset += sizeof(void*)) {
      uint8_t* guess = p_gDvm + offset;
      if (loadedJavaVM == *(JavaVM**)guess) {
        LOGI("Found vmList at offset %d.", offset);
        // Found it.  Now offset to find the LinearAllocHdr.
        LinearAllocHdr* linearAlloc = *(LinearAllocHdr**)(guess + javavmToLinearAllocOffset);
        if (checkLinearAlloc(linearAlloc, checkArgs, CHECK_VERBOSE)) {
          LOGI("Found LinearAllocHdr at expected offset from vmList.");
          return linearAlloc;
        }

        // If we can't verify it, look around a bit.  Maybe the OEM added or removed an
        // intervening field.  (The stock 2.3.5 ROM for the AT&T Galaxy S II appears to
        // have added a single pointer-sized field, for example.)
        // The decision to start with negative extra offsets is not an accident.
        // In AOSP, all of the fields just before the LinearAllocHdr are pointers,
        // so we're less likely to dereference something that escapes our memory map
        // validation and causes a segfault.
        LOGI("Failed to find LinearAllocHdr at expected offset from vmList.  Looking nearby.");
        for (size_t extraOffset = -javavmToLinearAllocExtraChecks * sizeof(void*);
            extraOffset <= javavmToLinearAllocExtraChecks * sizeof(void*);
            extraOffset += sizeof(void*)) {
          if (extraOffset == 0) {
            // Already checked.
            continue;
          }
          linearAlloc = *(LinearAllocHdr**)(guess + javavmToLinearAllocOffset + extraOffset);
          if (checkLinearAlloc(linearAlloc, checkArgs, CHECK_VERBOSE)) {
            LOGI("Found LinearAllocHdr at extra offset %d from vmList.", extraOffset);
            return linearAlloc;
          }
        }
      }
    }

    // Attempt number 2: We failed to find vmList (or failed to find the LinearAllocHdr
    // near it), so fall back to scanning the entire segment of gDvm where it's expected.
    // Dereference every pointer-sized word and try to validate it as a LinearAllocHdr.
    // We're relying on our memory map very heavily to avoid a segfault here.
    LOGW("Failed to find vmList in gDvm.  Going to brute-force search for LinearAllocHdr.");
    for (int offset = javavmOffsetMin; offset < offsetLimit; offset += sizeof(void*)) {
      LinearAllocHdr* linearAlloc = *(LinearAllocHdr**)(p_gDvm + offset);
      if (checkLinearAlloc(linearAlloc, checkArgs, CHECK_SILENT)) {
        // Repeat the check just to get it in the log.
        checkLinearAlloc(linearAlloc, checkArgs, CHECK_VERBOSE);
        LOGI("Found LinearAllocHdr (with no vmList) at offset %d.", offset);
        return linearAlloc;
      }
    }
    LOGE("Failed to find LinearAllocHdr in gDvm.");
  } else {
    LOGE("Could not find gDvm.  Heap scan is the only option.");
  }

  // Attempt number 3: We failed to find the LinearAllocHdr in gDvm (or couldn't find
  // gDvm at all), so scan the entire heap looking for it.
  // We only scan the region that the memory map reports as "[heap]", not anonymous
  // mappings.  As an optimization, we scan for the mapAddr first, then perform
  // full validation once we find it.
  LOGI("Attempting full heap scan (%p-%p).", heapStart, heapEnd);
  for (void** candidate = heapStart; candidate + sizeof(void*) <= (void**)heapEnd; candidate++) {
    if (*candidate == mapAddr) {
      LOGI("Found value equal to address of LinearAlloc buffer at address %p.", candidate);
      LinearAllocHdr* linearAlloc = (LinearAllocHdr*)(
          (uint8_t*)candidate - offsetof(LinearAllocHdr, mapAddr));
      LOGI("Offsetting backwards gives a LinearAllocHdr candidate of %p.", linearAlloc);
      if (checkLinearAlloc(linearAlloc, checkArgs, CHECK_VERBOSE)) {
        LOGI("Found LinearAllocHdr via heap scan at %p.", linearAlloc);
        return linearAlloc;
      }
    }
  }

  // Total failure.
  LOGE("All attempts to find LinearAlloc header have failed.");
  return NULL;
}

/**
 * Check a LinearAllocHdr pointer for validity.  Returns true iff we are
 * highly confident that it points to a valid LinearAllocHdr.
 */
static bool checkLinearAlloc(
    LinearAllocHdr* linearAlloc,
    LinearAllocCheckArgs checkArgs,
    enum CheckVerbosity verbose) {
  if (verbose == CHECK_VERBOSE) {
    LOGD("Evaluating LinearAllocHdr candidate at %p.", linearAlloc);
  }

  // Go through all of the tests even if we get a failure so we can get better
  // diagnostics from the logs.
  bool success = true;

#define DO_TEST(expr, ...) \
  if (expr) { \
    if (verbose == CHECK_VERBOSE) LOGI(__VA_ARGS__); \
    success = false; \
  }

  DO_TEST((size_t)safeRangeFrom(checkArgs.memoryMap, linearAlloc) < sizeof(*linearAlloc),
    "Pointer does not map to a LinearAllocHdr-sized block of valid memory.  Fail.");

  // If that one failed, we can't safely perform the rest of the test.
  if (!success) {
    return false;
  }

  DO_TEST(linearAlloc->mapAddr != checkArgs.mapAddr,
    "mapAddr doesn't match (%p != %p).  Fail.", linearAlloc->mapAddr, checkArgs.mapAddr);

  DO_TEST(linearAlloc->curOffset < checkArgs.minAllocated,
    "curOffset is too small (%d < %d).  Fail.", linearAlloc->curOffset, checkArgs.minAllocated);

  DO_TEST(linearAlloc->curOffset > checkArgs.maxAllocated,
    "curOffset is too large (%d > %d).  Fail.", linearAlloc->curOffset, checkArgs.maxAllocated);

  DO_TEST(linearAlloc->firstOffset != checkArgs.firstOffset,
    "firstOffset is wrong (%d != %d).  Fail.", linearAlloc->firstOffset, checkArgs.firstOffset);

  DO_TEST((linearAlloc->mapLength & ((1<<20)-1)) != 0,
    "mapLength is not a multiple of 1MB (%d).  Fail.", linearAlloc->mapLength);

  DO_TEST(linearAlloc->mapLength < checkArgs.minBufferSize,
    "mapLength is less than 5MB (%d).  Fail.", linearAlloc->mapLength);

  DO_TEST(linearAlloc->mapLength > checkArgs.maxBufferSize,
    "mapLength is greater than 16MB (%d).  Fail.", linearAlloc->mapLength);

#undef DO_TEST
  return success;
}

void Java_com_facebook_dalvik_DalvikInternals_replaceLinearAllocBuffer(
    JNIEnv* env, jobject clazz,
    jlong linearAllocHeaderAddress,
    jint newBufferSize,
    jint systemPageSize) {
  (void)clazz;

  if (!initted && !init(env))
  {
    return;
  }

  // In the highly unlikely event that an OEM shipped a build with ENFORCE_READ_ONLY
  // enabled, writeRefCount is an array of 16-bit counts, one per page of the main
  // buffer.  We need to allocate our own properly-sized to replace it.  We set
  // the ref counts to SHRT_MAX/2 to minimize the chance of an overflow or underflow.
  // Most likely, ENFORCE_READ_ONLY will be disabled (it's hard-coded off in AOSP),
  // but in that case, writeRefCount will be an uninitialized pointer, so it is not
  // very easy for us to identify that case.  We could try harder, but it would only
  // save about 2kB of memory, which is not very valuable.
  int numPages = (newBufferSize + systemPageSize-1) / systemPageSize;
  short* writeRefCount = calloc(numPages, sizeof(short));
  if (writeRefCount == NULL) {
    throwIoException(env, "Could not allocate writeRefCount.");
    return;
  }
  for (int i = 0; i < numPages; i++) {
    writeRefCount[i] = SHRT_MAX/2;
  }

  void* bytes = mmap(NULL, (int)newBufferSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (bytes == MAP_FAILED) {
    free(writeRefCount);
    throwIoException(env, "Could not mmap buffer for LinearAlloc.");
    return;
  }

  // On first glance, it would appear that we just clobber mapAddr without
  // regard for existing allocations. We do. During the normal course of an
  // Android app's lifetime, Dalvik never tries to free this buffer. Its
  // assertions that check these values for sanity are disabled. The old buffer
  // is still around and frees just mark a bit in the allocation's header.
  // Also, their realloc isn't terribly clever, so this should be safe as well.
  LinearAllocHdr* linearAlloc = (LinearAllocHdr*)(uintptr_t)linearAllocHeaderAddress;
  pthread_mutex_lock(&linearAlloc->lock);
  linearAlloc->mapAddr = bytes;
  linearAlloc->mapLength = newBufferSize;
  // Leave curOffset in place, so the current buffer will just start allocating
  // where the old one left off.  This wastes some address space, but not
  // physical memory.
  linearAlloc->writeRefCount = writeRefCount;
  pthread_mutex_unlock(&linearAlloc->lock);
}

jint Java_com_facebook_dalvik_DalvikInternals_getLinearAllocUsage(
    JNIEnv* env, jobject clazz,
    jlong linearAllocHeaderAddress) {
  (void)clazz;

  // Don't need to init since we don't do any Java stuff here.
  (void)env;

  LinearAllocHdr* linearAlloc = (LinearAllocHdr*)(uintptr_t)linearAllocHeaderAddress;
  // No need to lock.  The integer should be atomic and we don't care if we get
  // a slightly stale value.
  int offset = linearAlloc->curOffset;
  return offset;
}

// Helper for finding and keeping class references.
static bool findClass(JNIEnv* env, jclass* clsp, const char* name) {
  jclass cls = (*env)->FindClass(env, name);
  if (cls == NULL) {
    return false;
  }
  *clsp = (*env)->NewGlobalRef(env, cls);
  if (*clsp == NULL) {
    return false;
  }
  return true;
}

// Helper for finding our custom functions.
static void findOptionalSymbol(void** dest, void* library, const char* symbol) {
  *dest = dlsym(library, symbol);
  if (*dest) {
    LOGI("Successfully looked up %s", symbol);
  } else {
    LOGW("Failed to look up %s", symbol);
  }
}

jint JNI_OnLoad(JavaVM* jvm, void* reserved) {
  (void)reserved;
  loadedJavaVM = jvm;

  return JNI_VERSION_1_2;
}

// Initialization.
int init(JNIEnv* env) {
  initted = true;

  // Need IllegalStateException for error handling
  if (!findClass(env, &clsIllegalStateException, "java/lang/IllegalStateException")) {
    return false;
  }

  // Pre-find some important exception classes.
  if (!findClass(env, &clsIOException, "java/io/IOException")) {
    (*env)->ThrowNew(env, clsIllegalStateException, "Unable to find IOException");
    return false;
  }
  if (!findClass(env, &clsNullPointerException, "java/lang/NullPointerException")) {
    (*env)->ThrowNew(env, clsIllegalStateException, "Unable to find NullPointerException");
    return false;
  }

  // Open libdvm and resolve some symbols.
  // TODO: Might need to make this path an argument.
  void* libdvm = dlopen("libdvm.so", RTLD_LAZY);
  if (libdvm == NULL) {
    LOGE("dlopen(libdvm) failed: %d", errno);
    (*env)->ThrowNew(env, clsIllegalStateException, "Failed to load libdvm");
    return false;
  }

  // Symbols that we actually expect to find.
  findOptionalSymbol((void**)&p_JNI_GetCreatedJavaVMs, libdvm, "JNI_GetCreatedJavaVMs");
  findOptionalSymbol((void**)&p_gDvm, libdvm, "gDvm");

  // Symbols that only exist on our custom builds.
  findOptionalSymbol((void**)&p_ladDumpProfiles, libdvm, "ladDumpProfiles");
  findOptionalSymbol((void**)&p_ladResetProfiles, libdvm, "ladResetProfiles");
  findOptionalSymbol((void**)&p_ladPrintHeaderInfo, libdvm, "ladPrintHeaderInfo");

  return true;
}
