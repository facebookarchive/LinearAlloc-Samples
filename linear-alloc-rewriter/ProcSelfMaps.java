/**
 * Copyright (c) 2012-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant 
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.dalvik;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.Reader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Utilities for parsing /proc/self/maps (or /prod/PID/maps).
 */
public class ProcSelfMaps {

  /**
   * Represents a single virtual address mapping.
   */
  public static class Mapping {
    public long getStartAddress() {
      return mStartAddress;
    }

    public long getEndAddress() {
      return mEndAddress;
    }

    public boolean isReadable() {
      return mIsReadable;
    }

    public boolean isWritable() {
      return mIsWritable;
    }

    /**
     * "Private" means copy-on-write, according to proc(5).
     */
    public boolean isPrivate() {
      return mIsPrivate;
    }

    /**
     * Might be the empty string.  Will not be null.
     */
    public String getFileName() {
      return mFileName;
    }

    private final long mStartAddress;
    private final long mEndAddress;
    private final boolean mIsReadable;
    private final boolean mIsWritable;
    private final boolean mIsPrivate;
    private final String mFileName;

    Mapping(
        long startAddress,
        long endAddress,
        boolean isReadable,
        boolean writable,
        boolean isPrivate,
        String fileName) {
      mStartAddress = startAddress;
      mEndAddress = endAddress;
      mIsReadable = isReadable;
      mIsWritable = writable;
      mIsPrivate = isPrivate;
      mFileName = fileName;
    }
  }

  private final List<Mapping> mMappings;

  private ProcSelfMaps(List<Mapping> mappings) {
    mMappings = mappings;
  }

  public static ProcSelfMaps newFromLines(List<String> lines) {
    List<Mapping> mappings = new ArrayList<Mapping>(lines.size());
    for (String line : lines) {
      mappings.add(parseLine(line));
    }
    return new ProcSelfMaps(mappings);
  }

  public static ProcSelfMaps newFromFile(File mapsFile) throws IOException {
    Reader fileReader = new FileReader(mapsFile);
    try {
      BufferedReader bufferedReader = new BufferedReader(fileReader);
      List<String> lines = new ArrayList<String>();
      String line;
      while ((line = bufferedReader.readLine()) != null) {
        lines.add(line);
      }

      return newFromLines(lines);
    } finally {
      fileReader.close();
    }
  }

  public static ProcSelfMaps newFromSelf() throws IOException {
    return newFromFile(new File("/proc/self/maps"));
  }

  private static Mapping parseLine(String line) {
    String[] fields = line.split(" +", 6);
    if (fields.length != 6) {
      throw parseError(line);
    }

    String[] addresses = fields[0].split("-");
    if (addresses.length != 2) {
      throw parseError(line);
    }
    long startAddress;
    long endAddress;
    try {
      startAddress = Long.parseLong(addresses[0], 16);
      endAddress = Long.parseLong(addresses[1], 16);
    } catch (NumberFormatException e) {
      throw parseError(line);
    }

    if (fields[1].length() != 4) {
      throw parseError(line);
    }

    boolean isReadable = parseFlag(fields[1].charAt(0), 'r', line);
    boolean isWritable = parseFlag(fields[1].charAt(1), 'w', line);
    // Be less strict with this flag, since I don't know if values other than 'p' and 's'
    // are possible.
    boolean isPrivate = fields[1].charAt(3) == 'p';

    String fileName = fields[5];

    return new Mapping(startAddress, endAddress, isReadable, isWritable, isPrivate, fileName);
  }

  private static boolean parseFlag(char c, char expected, String line) {
    if (c == expected) {
      return true;
    } else if (c == '-') {
      return false;
    } else {
      throw parseError(line);
    }
  }

  private static IllegalArgumentException parseError(String line) {
    return new IllegalArgumentException("Invalid /proc/self/maps line: '" + line + "'");
  }

  public List<Mapping> getAllMappings() {
    return Collections.unmodifiableList(mMappings);
  }

  public List<Mapping> getAllMappingsFor(String fileSubstring) {
    List<Mapping> matchingMappings = new ArrayList<Mapping>();
    for (Mapping mapping : mMappings) {
      if (mapping.getFileName().contains(fileSubstring)) {
        matchingMappings.add(mapping);
      }
    }
    return matchingMappings;
  }

  /**
   * Find the first mapping with a specific substring in its file name.
   *
   * @param fileSubstring the substring to look for in the file name.
   * @return The first Mapping, or null if none is found.
   */
  public Mapping getFirstMappingFor(String fileSubstring) {
    for (Mapping mapping : mMappings) {
      if (mapping.getFileName().contains(fileSubstring)) {
        return mapping;
      }
    }
    return null;
  }

  /**
   * Make a copy of this object that only contains readable segments.
   *
   * @return A new ProcSelfMaps instance.
   */
  public ProcSelfMaps getReadableSubset() {
    List<Mapping> readableMappings = new ArrayList<Mapping>();
    for (Mapping mapping : mMappings) {
      if (mapping.isReadable()) {
        readableMappings.add(mapping);
      }
    }
    return new ProcSelfMaps(readableMappings);
  }

  /**
   * Get a map of memory segments.  Adjacent segments are coalesced.
   * Data is returned as a flat array of longs that alternate as start/end pointers
   * for a segment.  This makes the data easier to consume by JNI code.
   *
   * @return The map.
   */
  public long[] getFlatMemoryMap() {
    List<Long> memMap = new ArrayList<Long>();
    long lastStart = -1;
    long lastEnd = -1;
    for (Mapping m : mMappings) {
      if (lastStart == -1) {
        lastStart = m.getStartAddress();
        lastEnd = m.getEndAddress();
      } else if (m.getStartAddress() == lastEnd) {
        lastEnd = m.getEndAddress();
      } else {
        memMap.add(lastStart);
        memMap.add(lastEnd);
        lastStart = m.getStartAddress();
        lastEnd = m.getEndAddress();
      }
    }
    if (lastStart != -1) {
      memMap.add(lastStart);
      memMap.add(lastEnd);
    }

    long[] ret = new long[memMap.size()];
    for (int i = 0; i < memMap.size(); i++) {
      ret[i] = memMap.get(i);
    }
    return ret;
  }
}
