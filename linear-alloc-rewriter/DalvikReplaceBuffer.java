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
import android.util.Log;

public class DalvikReplaceBuffer {

  /**
   * Unlikely to be any other value on Froyo/GB.
   */
  private static final int SYSTEM_PAGE_SIZE = 4096;

  private static final String LOG_TAG = "DalvikReplaceBuffer";

  private static String failureReason = "";
  private static Result resultValue = Result.NOT_ATTEMPTED;

  public static Result getResult() {
    return resultValue;
  }

  public static String getFailureString() {
    if (getResult() == Result.FAILURE) {
      return failureReason;
    } else {
      throw new IllegalStateException("No failure string is provided when the operation " +
          "did not fail.");
    }
  }

  private static int getPlatformDefaultSize() {
    int sdkInt = Build.VERSION.SDK_INT;
    if (sdkInt < Build.VERSION_CODES.HONEYCOMB) {
      return DalvikConstants.GINGERBREAD_LINEAR_ALLOC_BUFFER_SIZE; // And before...
    } else if (sdkInt < Build.VERSION_CODES.JELLY_BEAN) {
      return DalvikConstants.HONEYCOMB_LINEAR_ALLOC_BUFFER_SIZE; // And ICS...
    } else {
      return DalvikConstants.JELLY_BEAN_LINEAR_ALLOC_BUFFER_SIZE; // And beyond...
    }
  }

  /**
   * Replace the LinearAlloc buffer if required to meet the desired size.
   * <p>
   * This function does not throw on error, but instead sets the result internally which can
   * be queried with {@link #getResult()}.
   *
   * This method is not thread safe and must be called before any Java threads
   * (other than the UI thread) are started.
   *
   * @param dalvikLinearAllocType Holds the size of the requested replace type.
   * @return true if buffer was replaced.
   */
  public static boolean replaceBufferIfNecessary(DalvikLinearAllocType dalvikLinearAllocType) {
    int platformDefaultSize = getPlatformDefaultSize();
    if (platformDefaultSize < dalvikLinearAllocType.bufferSizeBytes) {
      replaceBuffer(dalvikLinearAllocType);
      return true;
    }
    return false;
  }

  /**
   * Replace the LinearAlloc buffer regardless of whether it is necessary to do so.
   *
   * This method is not thread safe and must be called before any Java threads
   * (other than the UI thread) are started.
   *
   * @see #replaceBufferIfNecessary(DalvikLinearAllocType)
   *
   * @param dalvikLinearAllocType Holds the size of the requested replace type.
   */
  public static void replaceBuffer(DalvikLinearAllocType dalvikLinearAllocType) {
    if (resultValue != Result.NOT_ATTEMPTED) {
      Log.e(LOG_TAG, "Multiple attempts to replace the buffer detected!");
      return;
    }

    if (!DalvikInternals.isNativeLibraryLoaded()) {
      resultValue = Result.NOT_ATTEMPTED_NATIVE_LIBRARY_NOT_LOADED;
      return;
    }

    int requestedBufferSize = dalvikLinearAllocType.bufferSizeBytes;

    try {
      long linearAllocHeaderAddress = DalvikInternals.findLinearAllocHeader();
      if (linearAllocHeaderAddress == 0) {
        // Make this an IOException just to centralize our error handling.
        throw new IOException("Failed to find LinearAllocHdr.");
      } else {
        DalvikInternals.replaceLinearAllocBuffer(
            linearAllocHeaderAddress,
            requestedBufferSize,
            SYSTEM_PAGE_SIZE);
        resultValue = Result.SUCCESS;
      }
    } catch (IOException e) {
      resultValue = Result.FAILURE;
      failureReason = e.getMessage();

      Log.e(LOG_TAG,
          "Failed to replace LinearAlloc buffer (at size " + requestedBufferSize + "). " +
              "Continuing with standard buffer.", e);
    }
  }

  public static enum Result {
    NOT_ATTEMPTED,
    NOT_ATTEMPTED_NATIVE_LIBRARY_NOT_LOADED,
    FAILURE,
    SUCCESS,
  }
}
