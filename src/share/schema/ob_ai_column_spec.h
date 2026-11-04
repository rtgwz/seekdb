/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef OB_AI_COLUMN_SPEC_H_
#define OB_AI_COLUMN_SPEC_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/encode/ob_base64_encode.h"

namespace oceanbase
{
namespace share
{

/**
 * Wire format of an AI column definition carried from the SQL resolver to the
 * schema service (seekdb competition task 3).
 *
 * The resolver knows the full definition (model, prompt, options) while the
 * schema service only sees ObTableSchema, so the definition travels as the
 * comment of the hidden "__ai_<col>_result_spec" column:
 *
 *   comment = "__aispec__" + base64( field_0 FS field_1 FS ... FS field_8 )
 *
 * Column ids are NOT part of the spec: both sides derive them from the live
 * table schema (result column "<col>", hidden columns "__ai_<col>_state",
 * "__ai_<col>_request", "__ai_<col>_result_spec"), which keeps the spec valid
 * regardless of where the final column ids are assigned.
 */
static const char *const AI_COLUMN_SPEC_PREFIX = "__aispec__";
static const char AI_COLUMN_SPEC_FS = '\x1f';

// field order pinned by serialize()/deserialize()
static const int64_t AI_COLUMN_SPEC_FIELD_COUNT = 9;

struct ObAiColumnSpec
{
  ObAiColumnSpec()
      : func_type_(0), sync_mode_(0), max_retries_(3), vector_dim_(-1)
  {
  }
  common::ObString column_name_;     // visible result column
  common::ObString ai_model_name_;
  common::ObString dep_columns_;     // comma separated dependency column names
  common::ObString normalized_expr_; // typed system prompt + expression
  common::ObString expr_str_;        // raw expression, e.g. AI_COMPLETE('m', content)
  int64_t func_type_;
  int64_t sync_mode_;
  int64_t max_retries_;
  int64_t vector_dim_;               // -1 when not a vector column
};

class ObAiColumnSpecCodec
{
public:
  static bool is_spec_comment(const common::ObString &comment)
  {
    return comment.length() > static_cast<int64_t>(strlen(AI_COLUMN_SPEC_PREFIX))
           && 0 == STRNCASECMP(comment.ptr(), AI_COLUMN_SPEC_PREFIX,
                                strlen(AI_COLUMN_SPEC_PREFIX));
  }

  /** Serialize spec into "prefix + base64(fields)" using `alloc` for `out`. */
  static int serialize(const ObAiColumnSpec &spec, common::ObIAllocator &alloc,
                       common::ObString &out)
  {
    int ret = common::OB_SUCCESS;
    common::ObSqlString payload;
    const int64_t pre = strlen(AI_COLUMN_SPEC_PREFIX);
    if (OB_FAIL(payload.append_fmt("%.*s%c%.*s%c%.*s%c%.*s%c%.*s%c%ld%c%ld%c%ld%c%ld",
            static_cast<int>(spec.column_name_.length()), spec.column_name_.ptr(), AI_COLUMN_SPEC_FS,
            static_cast<int>(spec.ai_model_name_.length()), spec.ai_model_name_.ptr(), AI_COLUMN_SPEC_FS,
            static_cast<int>(spec.dep_columns_.length()), spec.dep_columns_.ptr(), AI_COLUMN_SPEC_FS,
            static_cast<int>(spec.normalized_expr_.length()), spec.normalized_expr_.ptr(), AI_COLUMN_SPEC_FS,
            static_cast<int>(spec.expr_str_.length()), spec.expr_str_.ptr(), AI_COLUMN_SPEC_FS,
            spec.func_type_, AI_COLUMN_SPEC_FS,
            spec.sync_mode_, AI_COLUMN_SPEC_FS,
            spec.max_retries_, AI_COLUMN_SPEC_FS,
            spec.vector_dim_))) {
    } else {
      const int64_t enc_len = common::ObBase64Encoder::needed_encoded_length(payload.length()) + pre + 1;
      char *buf = static_cast<char *>(alloc.alloc(enc_len));
      if (OB_ISNULL(buf)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t pos = 0;
        MEMCPY(buf, AI_COLUMN_SPEC_PREFIX, pre);
        pos = pre;
        if (OB_FAIL(common::ObBase64Encoder::encode(
                reinterpret_cast<const uint8_t *>(payload.ptr()), payload.length(),
                buf + pos, enc_len - pos, pos))) {
        } else {
          buf[pos] = '\0';
          out.assign_ptr(buf, static_cast<int32_t>(pos));
        }
      }
    }
    return ret;
  }

  /** Deserialize a spec comment; strings point into memory from `alloc`. */
  static int deserialize(const common::ObString &comment, common::ObIAllocator &alloc,
                         ObAiColumnSpec &spec)
  {
    int ret = common::OB_SUCCESS;
    if (!is_spec_comment(comment)) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      const int64_t pre = strlen(AI_COLUMN_SPEC_PREFIX);
      const int64_t b64_len = comment.length() - pre;
      const int64_t dec_len = common::ObBase64Encoder::needed_decoded_length(b64_len) + 1;
      uint8_t *buf = static_cast<uint8_t *>(alloc.alloc(dec_len));
      if (OB_ISNULL(buf)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t pos = 0;
        if (OB_FAIL(common::ObBase64Encoder::decode(comment.ptr() + pre, b64_len,
                buf, dec_len, pos))) {
        } else if (OB_UNLIKELY(pos <= 0)) {
          ret = common::OB_INVALID_ARGUMENT;
        } else {
          // split by FS into the 9 fields
          common::ObString fields[AI_COLUMN_SPEC_FIELD_COUNT];
          int64_t nfields = 0;
          int64_t start = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i <= pos && nfields < AI_COLUMN_SPEC_FIELD_COUNT; ++i) {
            if (i == pos || buf[i] == static_cast<uint8_t>(AI_COLUMN_SPEC_FS)) {
              fields[nfields++].assign_ptr(reinterpret_cast<char *>(buf + start),
                                           static_cast<int32_t>(i - start));
              start = i + 1;
            }
          }
          if (OB_UNLIKELY(AI_COLUMN_SPEC_FIELD_COUNT != nfields)) {
            ret = common::OB_INVALID_ARGUMENT;
          } else {
            spec.column_name_ = fields[0];
            spec.ai_model_name_ = fields[1];
            spec.dep_columns_ = fields[2];
            spec.normalized_expr_ = fields[3];
            spec.expr_str_ = fields[4];
            char *end = nullptr;
            spec.func_type_ = strtoll(fields[5].ptr(), &end, 10);
            spec.sync_mode_ = strtoll(fields[6].ptr(), &end, 10);
            spec.max_retries_ = strtoll(fields[7].ptr(), &end, 10);
            spec.vector_dim_ = strtoll(fields[8].ptr(), &end, 10);
          }
        }
      }
    }
    return ret;
  }
};

} // namespace share
} // namespace oceanbase

#endif /* OB_AI_COLUMN_SPEC_H_ */
