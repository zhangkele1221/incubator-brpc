// validator_util.h
#pragma once

#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/reflection.h>
#include <regex>
#include <algorithm>
#include <sstream>
#include <butil/logging.h>

//#include "butil/logging.h"

//#include "proto/validator.pb.h"

#include "validator.pb.h"

namespace validator {

struct ValidateResult {
    bool is_valid;
    std::string msg;
    
    ValidateResult(bool valid, const std::string& message = "") 
        : is_valid(valid), msg(message) {}
};

class ValidatorUtil {
public:
    static ValidateResult Validate(const google::protobuf::Message& message) {
        const google::protobuf::Descriptor* descriptor = message.GetDescriptor();
        const google::protobuf::Reflection* reflection = message.GetReflection();
        
        for (int i = 0; i < descriptor->field_count(); i++) {
            const google::protobuf::FieldDescriptor* field = descriptor->field(i);
            ValidateResult result = ValidateField(message, field, reflection);
            if (!result.is_valid) {
                return result;
            }
        }
        return ValidateResult(true, "");
    }

private:
    static ValidateResult ValidateField(const google::protobuf::Message& message,
                                      const google::protobuf::FieldDescriptor* field,
                                      const google::protobuf::Reflection* reflection) {
        
        const auto& validate_rules = field->options().GetExtension(validator::Rule);
        
        if (field->is_repeated()) {
            return ValidateRepeatedField(message, field, reflection, validate_rules);
        } else {
            return ValidateSingleField(message, field, reflection, validate_rules);
        }
    }
    
    static ValidateResult ValidateSingleField(const google::protobuf::Message& message,
                                           const google::protobuf::FieldDescriptor* field,
                                           const google::protobuf::Reflection* reflection,
                                           const validator::ValidateRules& rules) {
        
        switch (field->cpp_type()) {
            case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                if (rules.has_int32()) {
                    return ValidateInt32(reflection->GetInt32(message, field), 
                                       field->name(), rules.int32());
                }
                break;
                
            case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                if (rules.has_int64()) {
                    return ValidateInt64(reflection->GetInt64(message, field), 
                                       field->name(), rules.int64());
                }
                break;
                
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                if (rules.has_uint32()) {
                    return ValidateUInt32(reflection->GetUInt32(message, field), 
                                        field->name(), rules.uint32());
                }
                break;
                
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                if (rules.has_uint64()) {
                    return ValidateUInt64(reflection->GetUInt64(message, field), 
                                        field->name(), rules.uint64());
                }
                break;
                
            case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
                if (rules.has_float_()) {
                    return ValidateFloat(reflection->GetFloat(message, field), 
                                      field->name(), rules.float_());
                }
                break;
                
            case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
                if (rules.has_double_()) {
                    return ValidateDouble(reflection->GetDouble(message, field), 
                                       field->name(), rules.double_());
                }
                break;
                
            case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
                if (rules.has_string()) {
                    return ValidateString(reflection->GetString(message, field), 
                                       field->name(), rules.string());
                }
                break;
                
            case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
                if (rules.has_enum_()) {
                    return ValidateEnum(reflection->GetEnum(message, field), 
                                     field->name(), rules.enum_());
                }
                break;
                
            case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
                // 递归校验嵌套消息
                return Validate(reflection->GetMessage(message, field));
                
            default:
                break;
        }
        return ValidateResult(true, "");
    }
    
    static ValidateResult ValidateRepeatedField(const google::protobuf::Message& message,
                                              const google::protobuf::FieldDescriptor* field,
                                              const google::protobuf::Reflection* reflection,
                                              const validator::ValidateRules& rules) {
        
        if (rules.has_array()) {
            int size = reflection->FieldSize(message, field);
            const auto& rule = rules.array();
            
            // 检查数组非空
            if (rule.not_empty() && size == 0) {
                return ValidateResult(false, field->name() + " cannot be empty");
            }
            
            // 检查最小长度
            if (rule.has_min_len() && size < rule.min_len()) {
                return ValidateResult(false, field->name() + " size must be at least " + 
                                    std::to_string(rule.min_len()));
            }
            
            // 检查最大长度
            if (rule.has_max_len() && size > rule.max_len()) {
                return ValidateResult(false, field->name() + " size cannot exceed " + 
                                    std::to_string(rule.max_len()));
            }
            
            // 递归校验数组元素（如果是消息类型）
            if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                for (int i = 0; i < size; i++) {
                    auto result = Validate(reflection->GetRepeatedMessage(message, field, i));
                    if (!result.is_valid) {
                        return result;
                    }
                }
            }
        }
        return ValidateResult(true, "");
    }
    
    // 各种类型的校验实现
    static ValidateResult ValidateInt32(int32_t value, const std::string& field_name,
                                      const validator::Int32Rule& rule) {
        if (rule.lt_rule_case() == validator::Int32Rule::kLt && value >= rule.lt()) {
            return ValidateResult(false, field_name + " must be less than " + std::to_string(rule.lt()));
        }
        if (rule.lte_rule_case() == validator::Int32Rule::kLte && value > rule.lte()) {
            return ValidateResult(false, field_name + " must be less than or equal to " + std::to_string(rule.lte()));
        }
        if (rule.gt_rule_case() == validator::Int32Rule::kGt && value <= rule.gt()) {
            return ValidateResult(false, field_name + " must be greater than " + std::to_string(rule.gt()));
        }
        if (rule.gte_rule_case() == validator::Int32Rule::kGte && value < rule.gte()) {
            return ValidateResult(false, field_name + " must be greater than or equal to " + std::to_string(rule.gte()));
        }
        if (!rule.in().empty() && std::find(rule.in().begin(), rule.in().end(), value) == rule.in().end()) {
            return ValidateResult(false, field_name + " value not in allowed range");
        }
        if (!rule.not_in().empty() && std::find(rule.not_in().begin(), rule.not_in().end(), value) != rule.not_in().end()) {
            return ValidateResult(false, field_name + " value in forbidden range");
        }
        return ValidateResult(true, "");
    }
    
    static ValidateResult ValidateInt64(int64_t value, const std::string& field_name,
                                      const validator::Int64Rule& rule) {
        // 实现类似ValidateInt32
        if (rule.lt_rule_case() == validator::Int64Rule::kLt && value >= rule.lt()) {
            return ValidateResult(false, field_name + " must be less than " + std::to_string(rule.lt()));
        }
        // ... 其他规则类似
        return ValidateResult(true, "");
    }
    
    static ValidateResult ValidateUInt32(uint32_t value, const std::string& field_name,
                                       const validator::UInt32Rule& rule) {
        // 实现类似ValidateInt32
        return ValidateResult(true, "");
    }
    
    static ValidateResult ValidateUInt64(uint64_t value, const std::string& field_name,
                                        const validator::UInt64Rule& rule) {
        // 实现类似ValidateInt32
        return ValidateResult(true, "");
    }
    
    static ValidateResult ValidateFloat(float value, const std::string& field_name,
                                     const validator::FloatRule& rule) {
        // 实现类似ValidateInt32
        return ValidateResult(true, "");
    }
    
    static ValidateResult ValidateDouble(double value, const std::string& field_name,
                                      const validator::DoubleRule& rule) {
        // 实现类似ValidateInt32
        return ValidateResult(true, "");
    }
    
    static ValidateResult ValidateString(const std::string& value, const std::string& field_name,
                                       const validator::StringRule& rule) {
        if (rule.not_empty() && value.empty()) {
            return ValidateResult(false, field_name + " cannot be empty");
        }
        if (rule.min_len_rule_case() == validator::StringRule::kMinLen && value.length() < rule.min_len()) {
            return ValidateResult(false, field_name + " length must be at least " + std::to_string(rule.min_len()));
        }
        if (rule.max_len_rule_case() == validator::StringRule::kMaxLen && value.length() > rule.max_len()) {
            return ValidateResult(false, field_name + " length cannot exceed " + std::to_string(rule.max_len()));
        }
        if (!value.empty() && !rule.regex_pattern().empty()) {
            std::regex pattern(rule.regex_pattern());
            if (!std::regex_match(value, pattern)) {
                return ValidateResult(false, field_name + " format invalid");
            }
        }
        return ValidateResult(true, "");
    }
    
    static ValidateResult ValidateEnum(const google::protobuf::EnumValueDescriptor* value,
                                     const std::string& field_name,
                                     const validator::EnumRule& rule) {
        if (!rule.in().empty()) {
            int enum_value = value->number();
            if (std::find(rule.in().begin(), rule.in().end(), enum_value) == rule.in().end()) {
                return ValidateResult(false, field_name + " value not allowed");
            }
        }
        return ValidateResult(true, "");
    }
};

} // namespace validator