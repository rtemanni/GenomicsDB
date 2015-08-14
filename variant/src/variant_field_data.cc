#include "variant_field_data.h"

std::unordered_map<std::type_index, VariantFieldTypeEnum> g_variant_field_type_index_to_enum = 
std::unordered_map<std::type_index, VariantFieldTypeEnum>{
  { std::type_index(typeid(void)), VARIANT_FIELD_VOID },
  { std::type_index(typeid(int)), VARIANT_FIELD_INT },
  { std::type_index(typeid(int64_t)), VARIANT_FIELD_INT64_T },
  { std::type_index(typeid(unsigned)), VARIANT_FIELD_UNSIGNED },
  { std::type_index(typeid(uint64_t)), VARIANT_FIELD_UINT64_T },
  { std::type_index(typeid(float)), VARIANT_FIELD_FLOAT },
  { std::type_index(typeid(double)), VARIANT_FIELD_DOUBLE },
  { std::type_index(typeid(std::string)), VARIANT_FIELD_STRING },
  { std::type_index(typeid(char)), VARIANT_FIELD_CHAR }
};

template<>
bool is_tiledb_missing_value(const float value)
{
  fi_union null_float;
  null_float.f = NULL_FLOAT;
  fi_union curr;
  curr.f = value;
  return (curr.i == null_float.i);      //bitwise equality
}

template<>
bool is_tiledb_missing_value(const double value)
{
  di_union null_double;
  null_double.d = NULL_DOUBLE;
  di_union curr;
  curr.d = value;
  return (curr.i == null_double.i);      //bitwise equality
}

template<>
bool is_tiledb_missing_value(const char value) { return (value == NULL_CHAR); }

template<>
bool is_tiledb_missing_value(const int value) { return (value == NULL_INT); }

template<>
bool is_tiledb_missing_value(const unsigned value) { return (static_cast<int>(value) == NULL_INT); }

template<>
bool is_tiledb_missing_value(const int64_t value) { return (value == NULL_INT64_T); }

template<>
bool is_tiledb_missing_value(const uint64_t value) { return (static_cast<int64_t>(value) == NULL_INT64_T); }
