/**
 * Copyright (c) 2012-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant 
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.dalvik;

import java.io.IOException;

import android.os.Build;

import com.facebook.soloader.SoLoader;

/**
 * Noninstantiable class that hosts various native methods used to interact with Dalvik.
 */
public class DalvikInternals {
  // Make noninstantiable.
  private DalvikInternals() {}

  private static boolean mLibraryLoaded = false;

  static {
    try {
      SoLoader.loadLibrary("fb_dalvik-internals");
      mLibraryLoaded = true;
    } catch (UnsatisfiedLinkError ex) {
      // Ignore the exception.
    }
  }

  /**
   * If there was no attempt to load library before, call to this function
   * will result in a such attempt. This function will then return result as described.
   *
   * @return true if native library was loaded successfully, false if library failed to be loaded.
   */
  public static boolean isNativeLibraryLoaded() {
    return mLibraryLoaded;
  }

  /**
   * Get a simple string, just to make sure JNI is working.
   *
   * @return A human-readable string
   */
  public static native String getTestString();

  /**
   * Dump profiles of LinearAlloc usage to files.
   *
   * @param stackProfileFile Name of file for stack-oriented profile.
   * @param classProfileFile Name of file for class-oriented profile.
   */
  public static native void dumpLinearAllocProfiles(
      String stackProfileFile, String classProfileFile);

  /**
   * Resets all state in the LinearAlloc profiles.
   */
  public static native void resetLinearAllocProfiles();

  /**
   * Dumps the location of the LinearAlloc header (and a few anchor points)
   * to logcat.  (Only works on custom builds with LinearAlloc debugging.)
   */
  public static native void printLinearAllocHeaderInfo();

  // "I have altered the fields. Pray I do not alter them further" - Darth OEM
  // In AOSP's GB, this is at 744 bytes but we look a little earlier in case
  // fields were removed by the OEM.
  // Make this a multiple of 8 in case we ever run on 64-bit.
  private static final int MIN_JAVAVM_OFFSET = 704;
  // On the GB emulator this struct is 1080 bytes. Allow some extra wiggle room
  // in case OEMs added fields.
  private static final int DALVIK_GLOBALS_SIZE = 1480;
  // We hope that nothing will be added in the narrow gap between vmList and
  // the allocator header pointer.  One field was added by commit cb3c542b
  // between Froyo and Gingerbread
  private static final int JAVAVM_TO_LINEARALLOC_OFFSET_FROYO = 20;
  private static final int JAVAVM_TO_LINEARALLOC_OFFSET_GINGERBREAD = 24;
  // Unfortunately, some OEMs do change the offset.  This is the number of "extra"
  // attempts we make to find the LinearAllocHdr on each side of the expected offset,
  // with increments of one pointer size.
  private static final int JAVAVM_TO_LINEAR_ALLOC_EXTRA_TRIES = 3;
  // I measured this around 1.85MB in the emulator, but let's be more lenient
  // in case some OEM made the zygote load less.
  private static final int MIN_EXPECTED_LINEARALLOC_USED = 512 * 1024;
  // This should top out at 5 MB, but let's be more lenient
  // in case some OEM increased the limit.
  private static final int MAX_EXPECTED_LINEARALLOC_USED = 16 * 1024 * 1024;
  // Minimum taken directly from AOSP.  Changed between Gingerbread and Honeycomb.
  // OEMs might have increased this, but probably not more than 2x the ICS vaue.
  // Currently, we just hard-code these values.  We could try to infer them from
  // looking at the memory map, but that seems more error-prone.
  private static final int MIN_EXPECTED_LINEARALLOC_SIZE = 5 * 1024 * 1024;
  private static final int MAX_EXPECTED_LINEARALLOC_SIZE = 16 * 1024 * 1024;
  // Taken from AOSP (assuming system page size of 4096)
  private static final int EXPECTED_FIRST_OFFSET = (8-4) + 4096;

  /**
   * Find the address of the LinearAllocHdr used for class loading.
   *
   * This method is not thread safe and must be called before any Java threads
   * (other than the UI thread) are started.
   *
   * @return Address of the LinearAllocHdr, or 0 if it could not be found.
   */
  public static long findLinearAllocHeader() throws IOException {
    ProcSelfMaps memoryMap = ProcSelfMaps.newFromSelf();
    long[] flatMemoryMap = memoryMap.getReadableSubset().getFlatMemoryMap();

    int javavmToLinearAllocOffset;
    switch (Build.VERSION.SDK_INT) {
      case Build.VERSION_CODES.FROYO:
        javavmToLinearAllocOffset = JAVAVM_TO_LINEARALLOC_OFFSET_FROYO;
        break;
      case Build.VERSION_CODES.GINGERBREAD:
      case Build.VERSION_CODES.GINGERBREAD_MR1:
        javavmToLinearAllocOffset = JAVAVM_TO_LINEARALLOC_OFFSET_GINGERBREAD;
        break;
      default:
        // We don't run on Eclair or lower, and we won't find it on Honeycomb or higher,
        // so just set to 0.
        javavmToLinearAllocOffset = 0;
    }

    // LinearAlloc can be broken into multiple mappings because Dalvik manually sets parts of it
    // as non-writable or non-accessible.  Just grab the first one and return its start address.
    // We could be strict in looking for "/dev/ashmem/dalvik-LinearAlloc (deleted)", but I think
    // that that would be more likely to cause a false negative than a simple check for
    // "LinearAlloc" is to cause a false positive.
    ProcSelfMaps.Mapping linearAllocMapping = memoryMap.getFirstMappingFor("LinearAlloc");
    if (linearAllocMapping == null) {
      throw new IllegalStateException("Could not find LinearAlloc memory mapping.");
    }

    long heapStart = 0;
    long heapEnd = 0;
    ProcSelfMaps.Mapping heapMapping = memoryMap.getFirstMappingFor("[heap]");
    if (heapMapping != null && heapMapping.isReadable()) {
      heapStart = heapMapping.getStartAddress();
      heapEnd = heapMapping.getEndAddress();
    }

    return nativeFindLinearAllocHeader(
        flatMemoryMap,
        MIN_JAVAVM_OFFSET,
        DALVIK_GLOBALS_SIZE,
        javavmToLinearAllocOffset,
        JAVAVM_TO_LINEAR_ALLOC_EXTRA_TRIES,
        MIN_EXPECTED_LINEARALLOC_USED,
        MAX_EXPECTED_LINEARALLOC_USED,
        heapStart,
        heapEnd,
        MIN_EXPECTED_LINEARALLOC_SIZE,
        MAX_EXPECTED_LINEARALLOC_SIZE,
        EXPECTED_FIRST_OFFSET,
        linearAllocMapping.getStartAddress()
    );
  }

  /**
   * Find the address of the LinearAllocHdr used for class loading.
   * Several techniques are used; see the implementation for details.
   *
   * @param flatMemoryMap Flat map of readable memory, from ProcSelfMaps.
   * @param javavmOffsetMin Minimum offset of "JavaVM* vmList" within gDvm.  Must be
   *                        a multiple of sizeof(void*).
   * @param javavmOffsetLimit Limit (past-the-end) of offset of vmList.
   * @param javavmToLinearAllocOffset Expected offset from "JavaVM* vmList" to the LinearAllocHdr.
   * @param javavmToLinearAllocExtraTries Number of extra attempts to make searching for the
   *                                      LinearAllocHdr, at pointer-sized offsets from the
   *                                      expected location.
   * @param minAllocated Minimum expected amount of LinearAlloc memory already allocated.
   * @param maxAllocated Maximum expected amount of LinearAlloc memory already allocated.
   * @param heapStart Starting address of the heap.
   * @param heapEnd Ending (past-the-end) address of the heap.
   * @param minBufferSize Minimum expected size of the LinearAlloc buffer.
   * @param maxBufferSize Maximum expected size of the LinearAlloc buffer.
   * @param firstOffset Expected value of "firstOffset" field in the LinearAllocHdr.
   * @param mapAddr Mapped address LinearAlloc buffer.
   *
   * @return Address of the LinearAllocHdr, or 0 if it could not be found.
   */
  private static native long nativeFindLinearAllocHeader(
      long[] flatMemoryMap,
      int javavmOffsetMin,
      int javavmOffsetLimit,
      int javavmToLinearAllocOffset,
      int javavmToLinearAllocExtraTries,
      int minAllocated,
      int maxAllocated,
      long heapStart,
      long heapEnd,
      int minBufferSize,
      int maxBufferSize,
      int firstOffset,
      long mapAddr)
    throws IOException;

  /**
   * Replace this LinearAlloc buffer with a larger one.
   *
   * This method is not thread safe and must be called before any Java threads
   * (other than the UI thread) are started.
   */
  public static native void replaceLinearAllocBuffer(
      long linearAllocHeaderAddress,
      int newBufferSize,
      int systemPageSize)
    throws IOException;

  /**
   * Find the amount of LinearAlloc space used by the current process.
   *
   * @param linearAllocHeaderAddress Address of the LinearAlloc header.
   * @return Amount of LinearAlloc space used in bytes.
   */
  public static native int getLinearAllocUsage(
      long linearAllocHeaderAddress);
}
