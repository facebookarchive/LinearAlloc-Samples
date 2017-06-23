/**
 * Copyright (c) 2012-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant 
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.dalvik;

/**
* Enum representing the allocation type. Contains the size of the allocation.
*/
public enum DalvikLinearAllocType {

  /**
   * LinearAlloc size to use for debug builds.
   */
  DEBUG_BUILD(DalvikConstants.JELLY_BEAN_LINEAR_ALLOC_BUFFER_SIZE),

  /**
   * LinearAlloc size to use for release builds, where proguard is presumed to be enabled.
   */
  RELEASE_BUILD(12 * 1024 * 1024),

  public final int bufferSizeBytes;

  DalvikLinearAllocType(int bufferSizeBytes) {
    this.bufferSizeBytes = bufferSizeBytes;
  }
}
