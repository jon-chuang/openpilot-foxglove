#include "capnp/compat/json.h"
#include "capnp/dynamic.h"
#include "capnp/schema.h"
#include "capnp/serialize.h"
#include "capnp/message.h"
#include "capnp/schema-loader.h"
#include "capnp/compat/json.capnp.h"
#include "emscripten/bind.h"
#include "emscripten/val.h"
#include "kj/common.h"
#include <string>
#include <iostream>

class SchemaTranscoder {
    public:

    // **SAFETY**: data must be word aligned
    SchemaTranscoder(std::string schemas) : json_codec_(), schemas_(schemas), loader_(), idx_to_id_() {
        // TODO: extract this common logic into a util
        // Round up the word-aligned size just in case
        kj::ArrayPtr<const capnp::word> raw_words(reinterpret_cast<const capnp::word*>(schemas_.data()), schemas_.length() / sizeof(capnp::word));

        while (raw_words.size() > 0) {
            capnp::schema::Node::Reader schema(capnp::readMessageUnchecked<capnp::schema::Node>(raw_words.begin()));
            // We assume the schemas are single-segment; the first word indicates number of segments (0) and first segment length.
            assert (((const uint32_t *)raw_words.begin())[0] == 0);
            auto schemaWordSize = schema.totalSize().wordCount + 1;
            raw_words = kj::ArrayPtr<const capnp::word>(raw_words.begin() + schemaWordSize, raw_words.end());

            auto id = schema.getId();
            idx_to_id_.push_back(id);
            loader_.load(schema);

            std::cout << "Loaded schema with ID: " << id << " of total size: " << schemaWordSize << ". size left: " << raw_words.size() << ".\n";
        }
    }

    // **SAFETY**: data must be word aligned
    std::vector<std::string> decodeMessagesToJson(std::string data, int schema_idx) {
        // Round up the word-aligned size just in case
        kj::ArrayPtr<const capnp::word> raw_words(reinterpret_cast<const capnp::word*>(data.data()), data.length() / sizeof(capnp::word));
        
        std::vector<std::string> ret;
        
        while(raw_words.size() > 0) {
            capnp::FlatArrayMessageReader reader(raw_words);
            kj::ArrayPtr<const capnp::word> msgptr(raw_words.begin(), reader.getEnd());
            capnp::FlatArrayMessageReader msg(msgptr);
            raw_words = kj::ArrayPtr<const capnp::word>(reader.getEnd(), raw_words.end());

            auto schema = loader_.get(idx_to_id_[schema_idx]).asStruct();

            capnp::DynamicStruct::Reader decoded = msg.getRoot<capnp::DynamicStruct>(schema);
            kj::String encoded = json_codec_.encode(decoded, schema);
            std::string str(encoded.cStr(), encoded.size());
            std::cout << "JSON string: " << str << " size left: " << raw_words.size() << ".\n";
            ret.push_back(str);
        }
        return ret;
    }

    // readMessageFromFile

    private:

    capnp::JsonCodec json_codec_;
    std::string schemas_;
    std::vector<uint64_t> idx_to_id_;
    capnp::SchemaLoader loader_;
};

emscripten::val dynamicValueToVal(capnp::DynamicValue::Reader value, capnp::Type type);

emscripten::val dynamicStructToVal(capnp::DynamicStruct::Reader structValue);


class CapnpTranscoder {
    public:

    // **SAFETY**: data must be word aligned
    CapnpTranscoder(std::string schema_bin) : schema_bin_(schema_bin), loader_(), display_name_to_id_() {
        // TODO: extract this common logic into a util
        // Round up the word-aligned size just in case
        kj::ArrayPtr<const capnp::word> raw_words(reinterpret_cast<const capnp::word*>(schema_bin_.data()), schema_bin_.length() / sizeof(capnp::word));

        capnp::FlatArrayMessageReader message(raw_words);
        assert(message.getEnd() == raw_words.end());

        auto reader = message.getRoot<capnp::schema::CodeGeneratorRequest>();
        for (auto node: reader.getNodes()) {
          loader_.load(node);
          auto displayName = node.getDisplayName();
          auto key = std::string(displayName.cStr(), displayName.size());
          if (!display_name_to_id_.insert(std::pair<std::string, uint64_t>(key, node.getId())).second) {
            throw std::invalid_argument("Cannot have multiple occurrences of the same schema display name: " + key);
          }
          std::cout << "Loaded schema with name: " << key << key.size() << " with ID: " << node.getId() << ".\n";
        }
    }

    /// Sets the schema that this transcoder will 
    void setSchema(std::string schema_name) {
      // Validate: can only set struct schemas for now
      auto id = display_name_to_id_[schema_name];
      auto node = loader_.get(id).getProto();
      std::cout << "Setting schema schema with name: " << node.getDisplayName().cStr() << " with ID: " << node.getId() << ".\n";
      current_schema_ = schema_name;
    }

    emscripten::val transcode(std::string data) {
      kj::ArrayPtr<const capnp::word> raw_words(reinterpret_cast<const capnp::word*>(data.data()), data.length() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader message(raw_words);
      assert(message.getEnd() == raw_words.end());

      // TODO: can we not `get` the schema every single time...? 
      auto schema = loader_.get(display_name_to_id_[current_schema_]).asStruct();
      auto decoded = message.getRoot<capnp::DynamicStruct>(schema);
      auto val = dynamicStructToVal(decoded);

      return val;
    }

    std::vector<emscripten::val> transcodeBatch(std::string data) {
      // Round up the word-aligned size just in case
      kj::ArrayPtr<const capnp::word> raw_words(reinterpret_cast<const capnp::word*>(data.data()), data.length() / sizeof(capnp::word));
      
      std::vector<emscripten::val> ret;
      
      while(raw_words.size() > 0) {
        capnp::FlatArrayMessageReader reader(raw_words);
        kj::ArrayPtr<const capnp::word> msgptr(raw_words.begin(), reader.getEnd());
        capnp::FlatArrayMessageReader message(msgptr);
        raw_words = kj::ArrayPtr<const capnp::word>(reader.getEnd(), raw_words.end());

        // TODO: can we not `get` the schema every single time...? 
        auto schema = loader_.get(display_name_to_id_[current_schema_]).asStruct();
        auto decoded = message.getRoot<capnp::DynamicStruct>(schema);
        ret.push_back(dynamicStructToVal(decoded));
        std::cout << "Deserialized " << msgptr.size() * sizeof(capnp::word) 
          << " bytes to javascript object. Bytes left " << raw_words.size() * sizeof(capnp::word) << ".\n";
      }
      return ret;
    }

    emscripten::val transcodeJson(std::string data) {
      capnp::JsonCodec codec;

      kj::ArrayPtr<const capnp::word> raw_words(reinterpret_cast<const capnp::word*>(data.data()), data.length() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader message(raw_words);
      assert(message.getEnd() == raw_words.end());

      // TODO: can we not `get` the schema every single time...? 
      auto schema = loader_.get(display_name_to_id_[current_schema_]).asStruct();
      auto decoded = message.getRoot<capnp::DynamicStruct>(schema);

      kj::String encoded = codec.encode(decoded, schema);
      std::string str(encoded.cStr(), encoded.size());
      std::cout << "JSON string: " << str << ".\n";
      auto val = emscripten::val::object();
      val.set("json", str);
      return val;
    }


    private:

    std::string current_schema_;
    std::string schema_bin_;
    capnp::SchemaLoader loader_;
    std::map<std::string, uint64_t> display_name_to_id_;
};

emscripten::val dynamicStructToVal(capnp::DynamicStruct::Reader structValue) {
    auto object = emscripten::val::object();
    auto nonUnionFields = structValue.getSchema().getNonUnionFields();

    KJ_STACK_ARRAY(bool, hasField, nonUnionFields.size(), 32, 128);

    for (auto i: kj::indices(nonUnionFields)) {
        hasField[i] = structValue.has(nonUnionFields[i], capnp::HasMode::NON_NULL);
    }

    auto which = structValue.which();
    bool unionFieldIsNull = false;

    KJ_IF_MAYBE(field, which) {
        unionFieldIsNull = !structValue.has(*field, capnp::HasMode::NON_NULL);
        // If null and not default, ignore
        if (unionFieldIsNull && field->getProto().getDiscriminantValue() != 0) {
            which = nullptr;
        }
    }

    // Handle non-union fields
    for (auto i: kj::indices(nonUnionFields)) {
        if (hasField[i]) {
            auto field = nonUnionFields[i];
            auto val = dynamicValueToVal(structValue.get(field), field.getType());
            auto key = kj::str(field.getProto().getName());
            object.set(std::string(key.cStr(), key.size()), val);
        }
    }
    // Handle union field
    if (which != nullptr) {
        // Union field not printed yet; must be last.
        auto unionField = KJ_ASSERT_NONNULL(which);
        if (unionFieldIsNull) {
            auto key = kj::str(unionField.getProto().getName());
            object.set(std::string(key.cStr(), key.size()), emscripten::val::null());
        } else {
            auto val = dynamicValueToVal(structValue.get(unionField), unionField.getType());
            auto key = kj::str(unionField.getProto().getName());
            object.set(std::string(key.cStr(), key.size()), val);
        }
    }
    return object;
}

emscripten::val dynamicValueToVal(capnp::DynamicValue::Reader value, capnp::Type type) {
  switch (type.which()) {
    case capnp::schema::Type::VOID:
      return emscripten::val::null();
    case capnp::schema::Type::BOOL:
      return emscripten::val(value.as<bool>());
    case capnp::schema::Type::INT8:
      return emscripten::val(value.as<signed char>());
    case capnp::schema::Type::INT16:
      return emscripten::val(value.as<short>());
    case capnp::schema::Type::INT32:
      return emscripten::val(value.as<int>());
    case capnp::schema::Type::UINT8:
      return emscripten::val(value.as<unsigned char>());
    case capnp::schema::Type::UINT16:
      return emscripten::val(value.as<unsigned short>());
    case capnp::schema::Type::UINT32:
      return emscripten::val(value.as<unsigned int>());
    case capnp::schema::Type::FLOAT32:
      return emscripten::val(value.as<float>());
    case capnp::schema::Type::FLOAT64:
      return emscripten::val(value.as<double>());
    case capnp::schema::Type::INT64: {
      auto val = (signed long)value.as<int64_t>();
      return emscripten::val(val);
    }
    case capnp::schema::Type::UINT64: {
      auto val = (unsigned long)value.as<uint64_t>();
      return emscripten::val(val);
    }
    case capnp::schema::Type::TEXT: {
      kj::String str = kj::str(value.as<capnp::Text>());
      return emscripten::val(std::string(str.cStr(), str.size()));
    }
    case capnp::schema::Type::DATA: {
      // Turn into array of byte values. Yep, this is pretty ugly. People really need to override
      // this with a handler.
      auto bytes = value.as<capnp::Data>();
      return emscripten::val(emscripten::typed_memory_view(bytes.size(), (const uint8_t*)bytes.begin()));
    }
    case capnp::schema::Type::LIST: {
      auto list = value.as<capnp::DynamicList>();
      auto elementType = type.asList().getElementType();
      auto out = emscripten::val::array();
      for (auto i: kj::indices(list)) {
        auto val = dynamicValueToVal(list[i], elementType);
        out.call<void>("push", val);
      }
      return out;
    }
    case capnp::schema::Type::ENUM: {
      // TODO: should we support ENUMs as strings? What is the standard for foxglove?
      auto e = (unsigned short) value.as<capnp::DynamicEnum>().getRaw();
      return emscripten::val(e);
    }
    case capnp::schema::Type::STRUCT: {
      auto v = value.as<capnp::DynamicStruct>();
      return dynamicStructToVal(v);
    }
    case capnp::schema::Type::INTERFACE:
      KJ_FAIL_REQUIRE("don't know how to JSON-encode capabilities; "
                      "please register a JsonCodec::Handler for this");
    case capnp::schema::Type::ANY_POINTER:
      KJ_FAIL_REQUIRE("don't know how to JSON-encode AnyPointer; "
                      "please register a JsonCodec::Handler for this");
  }
}

int test(std::string data) {
    size_t size(data.length() / sizeof(int));
    assert (size == 2);
    const int* ptr(reinterpret_cast<const int *>(data.data()));
    std::vector<int> v(ptr, ptr+(size_t)size);
    return v[0] + v[1];
}


void validateSampleMessage(const kj::Array<capnp::word>& serialized) {
    capnp::SchemaLoader loader;
    // auto schema = loader.load(capnp::capnp::schema::from<capnp::json::Value::Field>().getProto()).asStruct();
    auto schema = loader.load(capnp::readMessageUnchecked<capnp::schema::Node>(capnp::json::Value::Field::_capnpPrivate::schema->encodedNode)).asStruct();

    std::cout << "Creating reader\n";

    kj::ArrayPtr<const capnp::word> msgptr(serialized.begin(), serialized.size());
    capnp::FlatArrayMessageReader msg(msgptr);
    
    std::cout << "Loading dynamic\n";
    auto decoded = msg.getRoot<capnp::DynamicStruct>(schema);

    std::cout << "Loaded\n";

    auto fields = decoded.getSchema().getFields();

    std::cout << "Initial Checking FIELDs\n";
    for (auto i: kj::indices(fields)) {
        std::cout << "HAS FIELD " << i << "? " << decoded.has(fields[i], capnp::HasMode::NON_NULL) << " \n";
    }
}

std::string buildSampleMessages() {
    capnp::MallocMessageBuilder message;
    auto jsonField = message.initRoot<capnp::json::Value::Field>();
    jsonField.setName("hello world");
    auto value = jsonField.initValue();
    value.setNumber(42);
    kj::Array<capnp::word> serialized = messageToFlatArray(message);

    char* ptr((char *)serialized.begin());
    std::cout << "bytes of len " << serialized.size() * sizeof(capnp::word) << ":\n [";
    for (size_t i=0; i < serialized.size() * sizeof(capnp::word); i++) {
        std::cout << uint16_t(ptr[i]) << ", ";
    }
    std::cout << "]\n";
    validateSampleMessage(serialized);

    // Send the same msg twice
    std::string str((char *)serialized.begin(), serialized.size() * sizeof(capnp::word));
    str.append(std::string((char *)serialized.begin(), serialized.size() * sizeof(capnp::word)));
    // TODO: convert this into a typed_memory_view byte array as below.
    return str;
}

emscripten::val buildSampleSchemas() {
    std::string ret;
    {
        auto serialized = capnp::Schema::from<capnp::json::Value::Field>().asUncheckedMessage();
        ret.append(std::string((char *)serialized.begin(), serialized.size() * sizeof(capnp::word)));
    }

    {
        auto serialized = capnp::Schema::from<capnp::json::Value>().asUncheckedMessage();
        ret.append(std::string((char *)serialized.begin(), serialized.size() * sizeof(capnp::word)));
    }

    return emscripten::val(emscripten::typed_memory_view(ret.length(), (const uint8_t *)ret.data()));
}

EMSCRIPTEN_BINDINGS(SchemaTranscoder) {
    emscripten::class_<SchemaTranscoder>("SchemaTranscoder")
        .constructor<std::string>()
        .function("decodeMessagesToJson", &SchemaTranscoder::decodeMessagesToJson);
    
    emscripten::register_vector<std::string>("vector<string>");
    emscripten::register_vector<emscripten::val>("vector<val>");
    emscripten::function("test", &test);
    emscripten::function("buildSampleMessages", &buildSampleMessages);
    emscripten::function("buildSampleSchemas", &buildSampleSchemas);

    emscripten::class_<CapnpTranscoder>("CapnpTranscoder")
        .constructor<std::string>()
        .function("transcodeJson", &CapnpTranscoder::transcodeJson)
        .function("transcode", &CapnpTranscoder::transcode)
        .function("transcodeBatch", &CapnpTranscoder::transcodeBatch)
        .function("setSchema", &CapnpTranscoder::setSchema);
}