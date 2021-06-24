#ifndef PYBIND11_PROTOBUF_FAST_CPP_PROTO_CASTERS_H_
#define PYBIND11_PROTOBUF_FAST_CPP_PROTO_CASTERS_H_

#include <Python.h>
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "pybind11_protobuf/enum_type_caster.h"
#include "pybind11_protobuf/proto_cast_util.h"

// pybind11 type_caster that works in conjunction with python fast_cpp_proto
// implementation to convert protocol buffers between C++ and python.
// This binder supports binaries linked with both native python protos
// and fast cpp python protos.
//
// Supports by value, by const reference and unique_ptr types.
// TODO: Enable std::shared_ptr of proto types.
//
// Casting to `const ProtoType&` works without copying in most cases,
// no submessage references should be held across language callsites,
// since those submessage references may become invalid.
//
// When returning a ::google::protobuf::Message (or derived class), python sees a
// concrete type based on the message descriptor.
//
// Passing dynamically generated message types is not yet supported.
//
// To use fast_cpp_proto_casters, include this file in the binding definition
// file.
//
// Example:
//
// #include <pybind11/pybind11.h>
// #include "pybind11_protobuf/fast_cpp_proto_casters.h"
//
// MyMessage GetMessage() { ... }
// PYBIND11_MODULE(my_module, m) {
//  m.def("get_message", &GetMessage);
// }
//

// WARNING   WARNING   WARNING   WARNING   WARNING   WARNING  WARNING
//
// This is still a work in progress.
//
// Sharing the same C++ protocol buffers with python is dangerous. They are
// currently permitted when return_value_policy::reference is used in a def(),
// def_property(), and similar pybind11 constructs. Such usage may lead multiple
// python objects pointing to the same C++ object (there is ongoing work to
// address this), conflicting mutations from python and C++, C++ code deleting
// an in-use python object, other potentially unsafe practices.

// NOTE: This is is incompatible with proto_casters.h in the same directory.
#if defined(PYBIND11_PROTOBUF_PROTO_CASTERS_H_)
#error "fast_cpp_proto_casters.h and proto_casters.h conflict."
#endif

// Enables unsafe conversions; currently these are a work in progress.
#if !defined(PYBIND11_PROTOBUF_UNSAFE)
#define PYBIND11_PROTOBUF_UNSAFE 0
#endif

namespace pybind11::google {

// pybind11 constructs c++ references using the following mechanism, for
// example:
//
// type_caster<T> caster;
// caster.load(handle, /*convert=*/ false);
// call(pybind11::detail::cast_op<const T&>(caster));
//
template <typename ProtoType>
struct proto_caster_load_impl {
  static_assert(
      std::is_same<ProtoType, pybind11::detail::intrinsic_t<ProtoType>>::value,
      "");

  // load converts from Python -> C++
  bool load(handle src, bool convert) {
    // When given a none, treat it as a nullptr.
    if (src.is_none()) {
      value = nullptr;
      return true;
    }

    // Use the PyProto_API to get an underlying C++ message pointer from the
    // object, which returns non-null when the incoming proto message
    // is a fast_cpp_proto instance.
    if (const ::google::protobuf::Message *message =
            pybind11_protobuf::PyProtoGetCppMessagePointer(src);
        message != nullptr) {
      if (ProtoType::default_instance().GetReflection() !=
          message->GetReflection()) {
        // Reflection type mismatch; from a different pool?
        return false;
      }
      // NOTE: We might need to know whether the proto has extensions that
      // are python-only here.
      //
      // If the capability were available, then we could probe PyProto_API and
      // allow c++ mutability based on the python reference count.

      value = static_cast<const ProtoType *>(message);
      return true;
    }

    // The incoming object is not a fast_cpp_proto, so attempt to
    // serialize it and deserialize into a native C++ proto type.
    auto descriptor_name = pybind11_protobuf::PyProtoDescriptorName(src);
    if (!descriptor_name ||
        *descriptor_name != ProtoType::descriptor()->full_name()) {
      // type mismatch.
      return false;
    }
    owned = std::unique_ptr<ProtoType>(new ProtoType());
    value = owned.get();
    return pybind11_protobuf::PyProtoCopyToCProto(src, owned.get());
  }

  // as_unique_ptr returns a copy of the object owned by a std::unique_ptr<T>,
  // which is suitable for move_only_holder_caster specializations.
  std::unique_ptr<ProtoType> as_unique_ptr() {
    if (!value) return nullptr;
    if (!owned) {
      owned = std::unique_ptr<ProtoType>(value->New());
      *owned = *value;
    }
    return std::move(owned);
  }

  const ProtoType *value;
  std::unique_ptr<ProtoType> owned;
};

template <>
struct proto_caster_load_impl<::google::protobuf::Message> {
  using ProtoType = ::google::protobuf::Message;

  bool load(handle src, bool convert) {
    if (src.is_none()) {
      value = nullptr;
      return true;
    }

    if (value = pybind11_protobuf::PyProtoGetCppMessagePointer(src);
        value != nullptr) {
      return true;
    }

    auto descriptor_name = pybind11_protobuf::PyProtoDescriptorName(src);
    if (!descriptor_name) {
      return false;
    }

    owned = pybind11_protobuf::AllocateCProtoByName(*descriptor_name);
    if (!owned) {
      // NOTE: This is a dynamic proto, or at least one that doesn't exist in
      // the C++ default pool. To import we need to do the equivalent of:
      //   file_proto = descriptor_pb2.FileDescriptorProto()
      //   src.DESCRIPTOR.file.CopyToProto(file_proto)
      //   descriptor_pool.Add(file_proto)
      //
      // And retry creating the object.
      return false;
    }
    value = owned.get();
    return pybind11_protobuf::PyProtoCopyToCProto(src, owned.get());
  }

  // as_unique_ptr returns a copy of the object owned by a std::unique_ptr<T>,
  // which is suitable for move_only_holder_caster specializations.
  std::unique_ptr<::google::protobuf::Message> as_unique_ptr() {
    if (!value) return nullptr;
    if (!owned) {
      owned = std::unique_ptr<::google::protobuf::Message>(value->New());
      owned->CopyFrom(*value);
    }
    return std::move(owned);
  }

  const ::google::protobuf::Message *value;
  std::unique_ptr<::google::protobuf::Message> owned;
};

struct fast_cpp_cast_impl {
  inline static handle cast_impl(::google::protobuf::Message *src,
                                 return_value_policy policy, handle parent,
                                 bool is_const) {
    if (src == nullptr) return none().release();

    if (is_const && (policy == return_value_policy::reference ||
                     policy == return_value_policy::reference_internal)) {
      throw type_error(
          "Cannot return a const reference to a ::google::protobuf::Message derived "
          "type.  Consider setting return_value_policy::copy in the "
          "pybind11 def().");
    }

    return pybind11_protobuf::GenericFastCppProtoCast(src, policy, parent,
                                                      is_const);
  }
};

struct native_cast_impl {
  inline static handle cast_impl(::google::protobuf::Message *src,
                                 return_value_policy policy, handle parent,
                                 bool is_const) {
    if (src == nullptr) return none().release();

    // When using native casters, always copy the proto.
    return pybind11_protobuf::GenericProtoCast(src, return_value_policy::copy,
                                               parent, false);
  }
};

// pybind11 type_caster specialization for c++ protocol buffer types.
template <typename ProtoType, typename CastBase>
struct proto_caster : public proto_caster_load_impl<ProtoType>,
                      public CastBase {
 private:
  using Loader = proto_caster_load_impl<ProtoType>;
  using CastBase::cast_impl;
  using Loader::owned;
  using Loader::value;

 public:
  static constexpr auto name = pybind11::detail::_<ProtoType>();

  // cast converts from C++ -> Python
  static handle cast(ProtoType &&src, return_value_policy policy,
                     handle parent) {
    return cast_impl(&src, return_value_policy::copy, parent, false);
  }

  static handle cast(const ProtoType *src, return_value_policy policy,
                     handle parent) {
    if (policy == return_value_policy::automatic ||
        policy == return_value_policy::automatic_reference) {
      policy = return_value_policy::copy;
    }
    return cast_impl(const_cast<ProtoType *>(src), policy, parent, true);
  }

  static handle cast(ProtoType *src, return_value_policy policy,
                     handle parent) {
    if (policy == return_value_policy::automatic ||
        policy == return_value_policy::automatic_reference) {
      policy = return_value_policy::copy;
    }
    std::unique_ptr<ProtoType> wrapper;
    if (policy == return_value_policy::take_ownership) {
      wrapper.reset(src);
      policy = return_value_policy::copy;
    }
    return cast_impl(src, policy, parent, false);
  }

  static handle cast(ProtoType const &src, return_value_policy policy,
                     handle parent) {
    if (policy == return_value_policy::automatic ||
        policy == return_value_policy::automatic_reference) {
      policy = return_value_policy::copy;
    }
    return cast_impl(const_cast<ProtoType *>(&src), return_value_policy::copy,
                     parent, true);
  }

  static handle cast(ProtoType &src, return_value_policy policy,
                     handle parent) {
    if (policy == return_value_policy::automatic ||
        policy == return_value_policy::automatic_reference) {
      policy = return_value_policy::copy;
    }
    return cast_impl(&src, policy, parent, false);
  }

  // PYBIND11_TYPE_CASTER
  explicit operator const ProtoType *() { return value; }
  explicit operator const ProtoType &() {
    if (!value) throw reference_cast_error();
    return *value;
  }
  explicit operator ProtoType &&() && {
    if (!value) throw reference_cast_error();
    if (!owned) {
      owned.reset(value->New());
      owned->CopyFrom(*value);
    }
    return std::move(*owned);
  }

#if PYBIND11_PROTOBUF_UNSAFE
  // The following unsafe conversions are not enabled:
  explicit operator ProtoType *() { return const_cast<ProtoType *>(value); }
  explicit operator ProtoType &() {
    if (!value) throw reference_cast_error();
    return *const_cast<ProtoType *>(value);
  }
#endif

  // cast_op_type determines which operator overload to call for a given c++
  // input parameter type.
  // clang-format off
  template <typename T_>
  using cast_op_type =
      std::conditional_t<
          std::is_same_v<std::remove_reference_t<T_>, const ProtoType *>,
              const ProtoType *,
      std::conditional_t<
          std::is_same_v<std::remove_reference_t<T_>, ProtoType *>, ProtoType *,
      std::conditional_t<
          std::is_same_v<T_, const ProtoType &>, const ProtoType &,
      std::conditional_t<std::is_same_v<T_, ProtoType &>, ProtoType &,
      /*default is T&&*/ T_>>>>;
  // clang-format on
};

}  // namespace pybind11::google
namespace pybind11::detail {

// pybind11 type_caster<> specialization for c++ protocol buffer types using
// inheritance from google::proto_caster<>.
template <typename ProtoType>
struct type_caster<
    ProtoType, std::enable_if_t<std::is_base_of_v<::google::protobuf::Message, ProtoType>>>
    : public google::proto_caster<ProtoType, google::fast_cpp_cast_impl> {};

// NOTE: If smart_holders becomes the default we will need to change this to
//    type_caster<std::unique_ptr<ProtoType, D>, ...
// Until then using that form is ambiguous due to the existing specialization
// that does *not* forward a sfinae clause. Or we could add an sfinae clause
// to the existing specialization, but that's a *much* larger change.
// Anyway, the existing specializations fully forward to these.

// move_only_holder_caster enables using move-only holder types such as
// std::unique_ptr. It uses type_caster<Proto> to manage the conversion
// and construct a holder type.
template <typename ProtoType, typename HolderType>
struct move_only_holder_caster<
    ProtoType, HolderType,
    std::enable_if_t<std::is_base_of_v<::google::protobuf::Message, ProtoType>>> {
 private:
  using Base = type_caster<intrinsic_t<ProtoType>>;
  static constexpr bool const_element =
      std::is_const_v<typename HolderType::element_type>;

 public:
  static constexpr auto name = Base::name;

  // C++->Python.
  static handle cast(HolderType &&src, return_value_policy, handle p) {
    auto *ptr = holder_helper<HolderType>::get(src);
    if (!ptr) return none().release();
    return Base::cast(std::move(*ptr), return_value_policy::move, p);
  }

  // Convert Python->C++.
  bool load(handle src, bool convert) {
    Base base;
    if (!base.load(src, convert)) {
      return false;
    }
    holder = base.as_unique_ptr();
    return true;
  }

  // PYBIND11_TYPE_CASTER
  explicit operator HolderType *() { return &holder; }
  explicit operator HolderType &() { return holder; }
  explicit operator HolderType &&() && { return std::move(holder); }

  template <typename T_>
  using cast_op_type = pybind11::detail::movable_cast_op_type<T_>;

 protected:
  HolderType holder;
};

// copyable_holder_caster enables using copyable holder types such as
// std::shared_ptr. It uses type_caster<Proto> to manage the conversion
// and construct a copy of the proto, then returns the shared_ptr.
//
// NOTE: When using pybind11 bindings, std::shared_ptr<Proto> is almost
// never correct, as it always makes a copy. It's mostly useful for handling
// methods that return a shared_ptr<const T>, which the caller never intends
// to mutate and where copy semantics will work just as well.
//
template <typename ProtoType, typename HolderType>
struct copyable_holder_caster<
    ProtoType, HolderType,
    std::enable_if_t<std::is_base_of_v<::google::protobuf::Message, ProtoType>>> {
 private:
  using Base = type_caster<intrinsic_t<ProtoType>>;
  static constexpr bool const_element =
      std::is_const_v<typename HolderType::element_type>;

 public:
  static constexpr auto name = Base::name;

  // C++->Python.
  static handle cast(const HolderType &src, return_value_policy, handle p) {
    // The default path calls into cast_holder so that the holder/deleter
    // gets added to the proto. Here we just make a copy
    const auto *ptr = holder_helper<HolderType>::get(src);
    if (!ptr) return none().release();
    return Base::cast(*ptr, return_value_policy::copy, p);
  }

  // Convert Python->C++.
  bool load(handle src, bool convert) {
    Base base;
    if (!base.load(src, convert)) {
      return false;
    }
    // This always makes a copy, but it could, in some cases, grab a reference
    // and construct a shared_ptr, since the intention is clearly to mutate
    // the existing object...
    holder = base.as_unique_ptr();
    return true;
  }

  explicit operator ProtoType *() { return holder.get(); }
  explicit operator ProtoType &() { return *holder.get(); }
  explicit operator HolderType &() { return holder; }

  template <typename>
  using cast_op_type = HolderType &;

 protected:
  HolderType holder;
};

// NOTE: We also need to add support and/or test classes:
//
//  ::google::protobuf::Descriptor
//  ::google::protobuf::EnumDescriptor
//  ::google::protobuf::EnumValueDescriptor
//  ::google::protobuf::FieldDescriptor
//

}  // namespace pybind11::detail

#endif  // PYBIND11_PROTOBUF_FAST_CPP_PROTO_CASTERS_H_
