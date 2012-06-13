// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#include "delimited-text-parser.h"

namespace impala {

// Updates the values in the field and tuple masks, escaping them if necessary.
// If the character at n is an escape character, then delimiters(tuple/field/escape
// characters) at n+1 don't count.
inline void ProcessEscapeMask(uint16_t escape_mask, bool* last_char_is_escape,
                              uint16_t* delim_mask) {
  // Escape characters can escape escape characters.
  bool first_char_is_escape = *last_char_is_escape;
  bool escape_next = first_char_is_escape;
  for (int i = 0; i < SSEUtil::CHARS_PER_128_BIT_REGISTER; ++i) {
    if (escape_next) {
      escape_mask &= ~SSEUtil::SSE_BITMASK[i];
    }
    escape_next = escape_mask & SSEUtil::SSE_BITMASK[i];
  }

  // Remember last character for the next iteration
  *last_char_is_escape = escape_mask &
    SSEUtil::SSE_BITMASK[SSEUtil::CHARS_PER_128_BIT_REGISTER - 1];

  // Shift escape mask up one so they match at the same bit index as the tuple and
  // field mask (instead of being the character before) and set the correct first bit
  escape_mask = escape_mask << 1 | (first_char_is_escape ? 1 : 0);

  // If escape_mask[n] is true, then tuple/field_mask[n] is escaped
  *delim_mask &= ~escape_mask;
}

template <bool process_escapes>
inline void DelimitedTextParser::AddColumn(int len, char** next_column_start, 
    int* num_fields, std::vector<FieldLocation>* field_locations) {
  if (ReturnCurrentColumn()) {
    DCHECK_LT(*num_fields, field_locations->size());
    // Found a column that needs to be parsed, write the start/len to 'field_locations'
    (*field_locations)[*num_fields].start = *next_column_start;
    if (process_escapes && current_column_has_escape_) {
      (*field_locations)[*num_fields].len = -len;
    } else {
      (*field_locations)[*num_fields].len = len;
    }
    ++(*num_fields);
  } 
  if (process_escapes) current_column_has_escape_ = false;
  *next_column_start += len + 1;
  ++column_idx_;
}

// SSE optimized raw text file parsing.  SSE4_2 added an instruction (with 3 modes) for
// text processing.  The modes mimic strchr, strstr and strcmp.  For text parsing, we can
// leverage the strchr functionality.
//
// The instruction operates on two sse registers:
//  - the needle (what you are searching for)
//  - the haystack (where you are searching in)
// Both registers can contain up to 16 characters.  The result is a 16-bit mask with a bit
// set for each character in the haystack that matched any character in the needle.
// For example:
//  Needle   = 'abcd000000000000' (we're searching for any a's, b's, c's or d's)
//  Haystack = 'asdfghjklhjbdwwc' (the raw string)
//  Result   = '1010000000011001'
template <bool process_escapes>
inline void DelimitedTextParser::ParseSse(int max_tuples, 
    int64_t* remaining_len, char** byte_buffer_ptr, 
    char** row_end_locations, 
    std::vector<FieldLocation>* field_locations,
    int* num_tuples, int* num_fields, char** next_column_start) {
  DCHECK(CpuInfo::Instance()->IsSupported(CpuInfo::SSE4_2));

  // To parse using SSE, we:
  //  1. Load into different sse registers the different characters we need to search for
  //        tuple breaks, field breaks, escape characters
  //  2. Load 16 characters at a time into the sse register
  //  3. Use the SSE instruction to do strchr on those 16 chars, the result is a bitmask
  //  4. Compute the bitmask for tuple breaks, field breaks and escape characters.
  //  5. If there are escape characters, fix up the matching masked bits in the
  //        field/tuple mask
  //  6. Go through the mask bit by bit and write the parsed data.
  
  // xmm registers:
  //  - xmm_buffer: the register holding the current (16 chars) we're working on from the
  //        file
  //  - xmm_delim_search_: the delim search register.  Contains field delimiter,
  //        collection_item delim_char and tuple delimiter
  //  - xmm_escape_search_: the escape search register. Only contains escape char
  //  - xmm_delim_mask: the result of doing strchr for the delimiters
  //  - xmm_escape_mask: the result of doing strchr for the escape char
  __m128i xmm_buffer, xmm_delim_mask, xmm_escape_mask;

  while (LIKELY(*remaining_len >= SSEUtil::CHARS_PER_128_BIT_REGISTER)) {
    // Load the next 16 bytes into the xmm register
    xmm_buffer = _mm_loadu_si128(reinterpret_cast<__m128i*>(*byte_buffer_ptr));

    // Do the strchr for tuple and field breaks
    // The strchr sse instruction returns the result in the lower bits of the sse
    // register.  Since we only process 16 characters at a time, only the lower 16 bits
    // can contain non-zero values.
    // _mm_extract_epi16 will extract 16 bits out of the xmm register.  The second
    // parameter specifies which 16 bits to extract (0 for the lowest 16 bits).
    xmm_delim_mask =
        _mm_cmpistrm(xmm_delim_search_, xmm_buffer, SSEUtil::STRCHR_MODE);
    uint16_t delim_mask = _mm_extract_epi16(xmm_delim_mask, 0);

    uint16_t escape_mask = 0;
    // If the table does not use escape characters, skip processing for it.
    if (process_escapes) {
      DCHECK(escape_char_ != '\0');
      xmm_escape_mask = _mm_cmpistrm(xmm_escape_search_, xmm_buffer,
                                    SSEUtil::STRCHR_MODE);
      escape_mask = _mm_extract_epi16(xmm_escape_mask, 0);
      ProcessEscapeMask(escape_mask, &last_char_is_escape_, &delim_mask);
    }

    int last_col_idx = 0;
    // Process all non-zero bits in the delim_mask from lsb->msb.  If a bit
    // is set, the character in that spot is either a field or tuple delimiter.
    while (delim_mask != 0) {
      // ffs is a libc function that returns the index of the first set bit (1-indexed)
      int n = ffs(delim_mask) - 1;
      DCHECK_GE(n, 0);
      DCHECK_LT(n, 16);
      // clear current bit
      delim_mask &= ~(SSEUtil::SSE_BITMASK[n]);

      if (process_escapes) {
        // Determine if there was an escape character between [last_col_idx, n]
        bool escaped = (escape_mask & low_mask_[last_col_idx] & high_mask_[n]) != 0;
        current_column_has_escape_ |= escaped;
        last_col_idx = n;
      }

      char* delim_ptr = *byte_buffer_ptr + n;

      AddColumn<process_escapes>(delim_ptr - *next_column_start,
          next_column_start, num_fields, field_locations);

      if ((*byte_buffer_ptr)[n] == tuple_delim_) {
        column_idx_ = scan_node_->num_partition_keys();
        row_end_locations[*num_tuples] = delim_ptr;
        ++(*num_tuples);
        if (UNLIKELY(*num_tuples == max_tuples)) {
          (*byte_buffer_ptr) += (n + 1);
          if (process_escapes) last_char_is_escape_ = false;
          *remaining_len += (n + 1);
          return;
        }
      }
    }

    if (process_escapes) {
      // Determine if there was an escape character between (last_col_idx, 15)
      bool unprocessed_escape = escape_mask & low_mask_[last_col_idx] & high_mask_[15];
      current_column_has_escape_ |= unprocessed_escape;
    }

    *remaining_len -= SSEUtil::CHARS_PER_128_BIT_REGISTER;
    *byte_buffer_ptr += SSEUtil::CHARS_PER_128_BIT_REGISTER;
  }
}

}

