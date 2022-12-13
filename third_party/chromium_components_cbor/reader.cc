// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/chromium_components_cbor/reader.h"

#include <math.h>

#include <utility>

#include "glog/logging.h"
#include "third_party/chromium_components_cbor/constants.h"
#include "third_party/chromium_base_numerics/checked_math.h"
#include "third_party/chromium_base_numerics/safe_conversions.h"

namespace cbor {

namespace constants {
const char kUnsupportedMajorType[] = "Unsupported major type.";
}

namespace {

Value::Type GetMajorType(uint8_t initial_data_byte) {
  return static_cast<Value::Type>(
      (initial_data_byte & constants::kMajorTypeMask) >>
      constants::kMajorTypeBitShift);
}

uint8_t GetAdditionalInfo(uint8_t initial_data_byte) {
  return initial_data_byte & constants::kAdditionalInformationMask;
}

// Error messages that correspond to each of the error codes. There is 1
// exception: we declare |kUnsupportedMajorType| in constants.h in the
// `constants` namespace, because we use it in several files.
const char kNoError[] = "Successfully deserialized to a CBOR value.";
const char kUnknownAdditionalInfo[] =
    "Unknown additional info format in the first byte.";
const char kIncompleteCBORData[] =
    "Prematurely terminated CBOR data byte array.";
const char kIncorrectMapKeyType[] =
    "Specified map key type is not supported by the current implementation.";
const char kTooMuchNesting[] = "Too much nesting.";
const char kInvalidUTF8[] =
    "String encodings other than UTF-8 are not allowed.";
const char kExtraneousData[] = "Trailing data bytes are not allowed.";
const char kMapKeyOutOfOrder[] =
    "Map keys must be strictly monotonically increasing based on byte length "
    "and then by byte-wise lexical order.";
const char kNonMinimalCBOREncoding[] =
    "Unsigned integers must be encoded with minimum number of bytes.";
const char kUnsupportedSimpleValue[] =
    "Unsupported or unassigned simple value.";
const char kUnsupportedFloatingPointValue[] =
    "Floating point numbers are not supported.";
const char kOutOfRangeIntegerValue[] =
    "Integer values must be between INT64_MIN and INT64_MAX.";
const char kUnknownError[] = "An unknown error occured.";

}  // namespace

Reader::Reader(std::vector<uint8_t> data)
    : rest_(data), error_code_(DecoderError::CBOR_NO_ERROR) {}
Reader::~Reader() {}

// static
absl::optional<Value> Reader::Read(std::vector<uint8_t> data,
                                   DecoderError* error_code_out,
                                   int max_nesting_level) {
  size_t num_bytes_consumed;
  auto value =
      Read(data, &num_bytes_consumed, error_code_out, max_nesting_level);

  if (value && num_bytes_consumed != data.size()) {
    if (error_code_out) {
      *error_code_out = DecoderError::EXTRANEOUS_DATA;
    }
    return absl::nullopt;
  }

  return value;
}

// static
absl::optional<Value> Reader::Read(std::vector<uint8_t> data,
                                   size_t* num_bytes_consumed,
                                   DecoderError* error_code_out,
                                   int max_nesting_level) {
  Reader reader(data);
  absl::optional<Value> value =
      reader.DecodeCompleteDataItem(max_nesting_level);

  auto error = reader.GetErrorCode();
  const bool success = value.has_value();
  DCHECK_EQ(success, error == DecoderError::CBOR_NO_ERROR);

  if (error_code_out) {
    *error_code_out = error;
  }

  *num_bytes_consumed =
      success ? data.size() - reader.num_bytes_remaining() : 0;
  return value;
}

absl::optional<Value> Reader::DecodeCompleteDataItem(int max_nesting_level) {
  if (max_nesting_level < 0 || max_nesting_level > kCBORMaxDepth) {
    error_code_ = DecoderError::TOO_MUCH_NESTING;
    return absl::nullopt;
  }

  absl::optional<DataItemHeader> header = DecodeDataItemHeader();
  if (!header.has_value()) {
    return absl::nullopt;
  }

  switch (header->type) {
    case Value::Type::UNSIGNED:
      return DecodeValueToUnsigned(header->value);
    case Value::Type::NEGATIVE:
      return DecodeValueToNegative(header->value);
    case Value::Type::BYTE_STRING:
      return ReadByteStringContent(*header);
    case Value::Type::STRING:
      return ReadStringContent(*header);
    case Value::Type::ARRAY:
      return ReadArrayContent(*header, max_nesting_level);
    case Value::Type::MAP:
      return ReadMapContent(*header, max_nesting_level);
    case Value::Type::SIMPLE_VALUE:
      return DecodeToSimpleValue(*header);
    case Value::Type::TAG:  // We explicitly don't support TAG.
    case Value::Type::NONE:
      break;
  }

  error_code_ = DecoderError::UNSUPPORTED_MAJOR_TYPE;
  return absl::nullopt;
}

absl::optional<Reader::DataItemHeader> Reader::DecodeDataItemHeader() {
  const absl::optional<uint8_t> initial_byte = ReadByte();
  if (!initial_byte) {
    return absl::nullopt;
  }

  const auto major_type = GetMajorType(initial_byte.value());
  const uint8_t additional_info = GetAdditionalInfo(initial_byte.value());

  absl::optional<uint64_t> value = ReadVariadicLengthInteger(additional_info);
  return value ? absl::make_optional(
                     DataItemHeader{major_type, additional_info, value.value()})
               : absl::nullopt;
}

absl::optional<uint64_t> Reader::ReadVariadicLengthInteger(
    uint8_t additional_info) {
  uint8_t additional_bytes = 0;
  if (additional_info < 24) {
    return absl::make_optional(additional_info);
  } else if (additional_info == 24) {
    additional_bytes = 1;
  } else if (additional_info == 25) {
    additional_bytes = 2;
  } else if (additional_info == 26) {
    additional_bytes = 4;
  } else if (additional_info == 27) {
    additional_bytes = 8;
  } else {
    error_code_ = DecoderError::UNKNOWN_ADDITIONAL_INFO;
    return absl::nullopt;
  }

  const absl::optional<std::vector<uint8_t>> bytes =
      ReadBytes(additional_bytes);
  if (!bytes) {
    return absl::nullopt;
  }

  uint64_t int_data = 0;
  for (const uint8_t b : bytes.value()) {
    int_data <<= 8;
    int_data |= b;
  }

  return IsEncodingMinimal(additional_bytes, int_data)
             ? absl::make_optional(int_data)
             : absl::nullopt;
}

absl::optional<Value> Reader::DecodeValueToNegative(uint64_t value) {
  auto negative_value = -base::CheckedNumeric<int64_t>(value) - 1;
  if (!negative_value.IsValid()) {
    error_code_ = DecoderError::OUT_OF_RANGE_INTEGER_VALUE;
    return absl::nullopt;
  }
  return Value(negative_value.ValueOrDie());
}

absl::optional<Value> Reader::DecodeValueToUnsigned(uint64_t value) {
  auto unsigned_value = base::CheckedNumeric<int64_t>(value);
  if (!unsigned_value.IsValid()) {
    error_code_ = DecoderError::OUT_OF_RANGE_INTEGER_VALUE;
    return absl::nullopt;
  }
  return Value(unsigned_value.ValueOrDie());
}

absl::optional<Value> Reader::DecodeToSimpleValue(
    const DataItemHeader& header) {
  // ReadVariadicLengthInteger provides this bound.
  CHECK_LE(header.additional_info, 27);
  // Floating point numbers are not supported.
  if (header.additional_info > 24) {
    error_code_ = DecoderError::UNSUPPORTED_FLOATING_POINT_VALUE;
    return absl::nullopt;
  }

  // Since |header.additional_info| <= 24, ReadVariadicLengthInteger also
  // provides this bound for |header.value|.
  CHECK_LE(header.value, 255u);
  // |SimpleValue| is an enum class and so the underlying type is specified to
  // be |int|. So this cast is safe.
  Value::SimpleValue possibly_unsupported_simple_value =
      static_cast<Value::SimpleValue>(static_cast<int>(header.value));
  switch (possibly_unsupported_simple_value) {
    case Value::SimpleValue::FALSE_VALUE:
    case Value::SimpleValue::TRUE_VALUE:
    case Value::SimpleValue::NULL_VALUE:
    case Value::SimpleValue::UNDEFINED:
      return Value(possibly_unsupported_simple_value);
  }

  error_code_ = DecoderError::UNSUPPORTED_SIMPLE_VALUE;
  return absl::nullopt;
}

absl::optional<Value> Reader::ReadStringContent(
    const Reader::DataItemHeader& header) {
  uint64_t num_bytes = header.value;
  const absl::optional<std::vector<uint8_t>> bytes = ReadBytes(num_bytes);
  if (!bytes) {
    return absl::nullopt;
  }

  std::string cbor_string(bytes->begin(), bytes->end());

  return HasValidUTF8Format(cbor_string)
             ? absl::make_optional<Value>(Value(std::move(cbor_string)))
             : absl::nullopt;
}

absl::optional<Value> Reader::ReadByteStringContent(
    const Reader::DataItemHeader& header) {
  uint64_t num_bytes = header.value;
  const absl::optional<std::vector<uint8_t>> bytes = ReadBytes(num_bytes);
  if (!bytes) {
    return absl::nullopt;
  }

  std::vector<uint8_t> cbor_byte_string(bytes->begin(), bytes->end());
  return Value(std::move(cbor_byte_string));
}

absl::optional<Value> Reader::ReadArrayContent(
    const Reader::DataItemHeader& header, int max_nesting_level) {
  const uint64_t length = header.value;

  Value::ArrayValue cbor_array;
  for (uint64_t i = 0; i < length; ++i) {
    absl::optional<Value> cbor_element =
        DecodeCompleteDataItem(max_nesting_level - 1);
    if (!cbor_element.has_value()) {
      return absl::nullopt;
    }
    cbor_array.push_back(std::move(cbor_element.value()));
  }
  return Value(std::move(cbor_array));
}

absl::optional<Value> Reader::ReadMapContent(
    const Reader::DataItemHeader& header, int max_nesting_level) {
  const uint64_t length = header.value;

  Value::MapValue cbor_map;
  for (uint64_t i = 0; i < length; ++i) {
    absl::optional<Value> key = DecodeCompleteDataItem(max_nesting_level - 1);
    absl::optional<Value> value = DecodeCompleteDataItem(max_nesting_level - 1);
    if (!key.has_value() || !value.has_value()) {
      return absl::nullopt;
    }

    switch (key.value().type()) {
      case Value::Type::UNSIGNED:
      case Value::Type::NEGATIVE:
      case Value::Type::STRING:
      case Value::Type::BYTE_STRING:
        break;
      default:
        error_code_ = DecoderError::INCORRECT_MAP_KEY_TYPE;
        return absl::nullopt;
    }
    if (!IsKeyInOrder(key.value(), &cbor_map)) {
      return absl::nullopt;
    }

    // cbor_map.insert_or_assign(std::move(key.value()), std::move(value.value()));
    cbor_map[std::move(key.value())] = std::move(value.value());
  }
  return Value(std::move(cbor_map));
}

absl::optional<uint8_t> Reader::ReadByte() {
  const absl::optional<std::vector<uint8_t>> bytes = ReadBytes(1);
  return bytes ? absl::make_optional(bytes.value()[0]) : absl::nullopt;
}

absl::optional<std::vector<uint8_t>> Reader::ReadBytes(uint64_t num_bytes) {
  if (static_cast<uint64_t>(rest_.size()) < num_bytes) {
    error_code_ = DecoderError::INCOMPLETE_CBOR_DATA;
    return absl::nullopt;
  }
  std::vector<uint8_t> ret(rest_.begin(), rest_.begin() + num_bytes);
  rest_ = std::vector<uint8_t>(rest_.begin() + num_bytes, rest_.end());
  return ret;
}

bool Reader::IsEncodingMinimal(uint8_t additional_bytes, uint64_t uint_data) {
  if ((additional_bytes == 1 && uint_data < 24) ||
      uint_data <= (1ULL << 8 * (additional_bytes >> 1)) - 1) {
    error_code_ = DecoderError::NON_MINIMAL_CBOR_ENCODING;
    return false;
  }
  return true;
}

bool Reader::HasValidUTF8Format(const std::string& string_data) {
  // TODO Use this for UTF-8 checks on the CBOR.
  return true;
}

bool Reader::IsKeyInOrder(const Value& new_key, Value::MapValue* map) {
  if (map->empty()) {
    return true;
  }

  const auto& max_current_key = map->rbegin()->first;
  const auto less = map->key_comp();
  if (!less(max_current_key, new_key)) {
    error_code_ = DecoderError::OUT_OF_ORDER_KEY;
    return false;
  }
  return true;
}

// static
const char* Reader::ErrorCodeToString(DecoderError error) {
  switch (error) {
    case DecoderError::CBOR_NO_ERROR:
      return kNoError;
    case DecoderError::UNSUPPORTED_MAJOR_TYPE:
      return constants::kUnsupportedMajorType;
    case DecoderError::UNKNOWN_ADDITIONAL_INFO:
      return kUnknownAdditionalInfo;
    case DecoderError::INCOMPLETE_CBOR_DATA:
      return kIncompleteCBORData;
    case DecoderError::INCORRECT_MAP_KEY_TYPE:
      return kIncorrectMapKeyType;
    case DecoderError::TOO_MUCH_NESTING:
      return kTooMuchNesting;
    case DecoderError::INVALID_UTF8:
      return kInvalidUTF8;
    case DecoderError::EXTRANEOUS_DATA:
      return kExtraneousData;
    case DecoderError::OUT_OF_ORDER_KEY:
      return kMapKeyOutOfOrder;
    case DecoderError::NON_MINIMAL_CBOR_ENCODING:
      return kNonMinimalCBOREncoding;
    case DecoderError::UNSUPPORTED_SIMPLE_VALUE:
      return kUnsupportedSimpleValue;
    case DecoderError::UNSUPPORTED_FLOATING_POINT_VALUE:
      return kUnsupportedFloatingPointValue;
    case DecoderError::OUT_OF_RANGE_INTEGER_VALUE:
      return kOutOfRangeIntegerValue;
    case DecoderError::UNKNOWN_ERROR:
      return kUnknownError;
    default:
      DCHECK(false);
      return "Unknown error code.";
  }
}

}  // namespace cbor
