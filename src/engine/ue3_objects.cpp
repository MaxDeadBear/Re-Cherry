#include "engine/ue3_objects.h"

#include "re_cherry_init.h"

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace cherry::engine::ue3 {

namespace {

// Offsets and globals are documented with their IDA sources in docs/reverse-engineering/ue3-script.md.
constexpr uint32_t kGObjObjects = 0x83367248;
constexpr uint32_t kGNames = 0x83367208;

constexpr uint32_t kObjectIndex = 0x04;
constexpr uint32_t kObjectFlags = 0x08;
constexpr uint32_t kObjectOuter = 0x28;
constexpr uint32_t kObjectName = 0x2C;
constexpr uint32_t kObjectClass = 0x34;
constexpr uint32_t kFieldNext = 0x3C;
constexpr uint32_t kStructSuper = 0x40;
constexpr uint32_t kStructChildren = 0x44;
constexpr uint32_t kStructScript = 0x4C;
constexpr uint32_t kClassCastFlags = 0xCC;
constexpr uint32_t kPropertyArrayDim = 0x40;
constexpr uint32_t kPropertyElementSize = 0x44;
constexpr uint32_t kPropertyOffset = 0x54;
constexpr uint32_t kPropertyExtra = 0x74;

constexpr uint32_t kNameEntryText = 8;
constexpr uint32_t kRfClassDefaultObject = 0x200;
constexpr uint32_t kMaxNameChars = 256;

uint8_t* Base() {
  return rex::system::kernel_state()->memory()->virtual_membase();
}

uint16_t GetU16(uint32_t address) {
  uint8_t* base = Base();
  return REX_LOAD_U16(address);
}

std::string NameText(uint32_t index) {
  if (index >= GetU32(kGNames + 4)) return "<bad name>";
  const uint32_t entry = GetU32(GetU32(kGNames) + index * 4);
  if (!entry) return "<null name>";
  const bool wide = GetU32(entry) & 1;
  std::string text;
  for (uint32_t i = 0; i < kMaxNameChars; ++i) {
    const uint32_t ch = wide ? GetU16(entry + kNameEntryText + i * 2) : GetU8(entry + kNameEntryText + i);
    if (!ch) break;
    text.push_back(ch <= 0x7F ? char(ch) : '?');
  }
  return text;
}

}  // namespace

uint32_t GetU32(uint32_t address) {
  uint8_t* base = Base();
  return REX_LOAD_U32(address);
}

void SetU32(uint32_t address, uint32_t value) {
  uint8_t* base = Base();
  REX_STORE_U32(address, value);
}

uint8_t GetU8(uint32_t address) {
  uint8_t* base = Base();
  return REX_LOAD_U8(address);
}

void SetU8(uint32_t address, uint8_t value) {
  uint8_t* base = Base();
  REX_STORE_U8(address, value);
}

uint32_t NameCount() {
  return GetU32(kGNames + 4);
}

uint32_t ObjectCount() {
  return GetU32(kGObjObjects + 4);
}

uint32_t ObjectAt(uint32_t index) {
  return index < ObjectCount() ? GetU32(GetU32(kGObjObjects) + index * 4) : 0;
}

uint32_t ObjectIndex(uint32_t object) {
  return GetU32(object + kObjectIndex);
}

bool IsLive(uint32_t object) {
  return object && ObjectAt(ObjectIndex(object)) == object;
}

uint32_t ObjectClass(uint32_t object) {
  return GetU32(object + kObjectClass);
}

uint32_t ObjectOuter(uint32_t object) {
  return GetU32(object + kObjectOuter);
}

std::string NameString(uint32_t index, uint32_t number) {
  std::string name = NameText(index);
  if (number) name += "_" + std::to_string(number - 1);
  return name;
}

std::string ObjectName(uint32_t object) {
  return object ? NameString(GetU32(object + kObjectName), GetU32(object + kObjectName + 4)) : "None";
}

std::string ObjectPath(uint32_t object) {
  std::string path = ObjectName(object);
  for (uint32_t outer = ObjectOuter(object), depth = 0; outer && depth < 16; outer = ObjectOuter(outer), ++depth) path = ObjectName(outer) + "." + path;
  return path;
}

bool IsDefaultObject(uint32_t object) {
  return GetU32(object + kObjectFlags + 4) & kRfClassDefaultObject;
}

bool IsA(uint32_t object, uint32_t cls) {
  for (uint32_t c = ObjectClass(object); c; c = SuperStruct(c))
    if (c == cls) return true;
  return false;
}

uint32_t ClassCastFlags(uint32_t cls) {
  return GetU32(cls + kClassCastFlags);
}

uint32_t SuperStruct(uint32_t structure) {
  return GetU32(structure + kStructSuper);
}

uint32_t FirstChild(uint32_t structure) {
  return GetU32(structure + kStructChildren);
}

uint32_t NextField(uint32_t field) {
  return GetU32(field + kFieldNext);
}

std::vector<uint8_t> Script(uint32_t structure) {
  const uint32_t data = GetU32(structure + kStructScript);
  const uint32_t size = data ? GetU32(structure + kStructScript + 4) : 0;
  std::vector<uint8_t> bytes(size);
  for (uint32_t i = 0; i < size; ++i) bytes[i] = GetU8(data + i);
  return bytes;
}

uint32_t FindClass(std::string_view name) {
  const uint32_t count = ObjectCount();
  uint32_t class_class = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t object = ObjectAt(i);
    if (!object) continue;
    const uint32_t cls = ObjectClass(object);
    if (!class_class) class_class = ObjectClass(cls);
    if (cls == class_class && ObjectName(object) == name) return object;
  }
  return 0;
}

std::vector<uint32_t> Instances(uint32_t cls) {
  std::vector<uint32_t> out;
  const uint32_t count = ObjectCount();
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t object = ObjectAt(i);
    if (object && !IsDefaultObject(object) && IsA(object, cls)) out.push_back(object);
  }
  return out;
}

Property ReadProperty(uint32_t property) {
  Property p;
  p.object = property;
  p.offset = GetU32(property + kPropertyOffset);
  p.array_dim = GetU32(property + kPropertyArrayDim);
  p.element_size = GetU32(property + kPropertyElementSize);
  p.cast_flags = ClassCastFlags(ObjectClass(property));
  p.extra = GetU32(property + kPropertyExtra);
  return p;
}

uint32_t FindField(uint32_t structure, std::string_view name) {
  for (uint32_t s = structure; s; s = SuperStruct(s))
    for (uint32_t field = FirstChild(s); field; field = NextField(field))
      if (ObjectName(field) == name) return field;
  return 0;
}

Property FindProperty(uint32_t structure, std::string_view name) {
  const uint32_t field = FindField(structure, name);
  return field && (ClassCastFlags(ObjectClass(field)) & kCastProperty) ? ReadProperty(field) : Property{};
}

bool GetBool(uint32_t object, const Property& property, uint32_t index) {
  return GetU32(object + property.offset + index * property.element_size) & property.extra;
}

void SetBool(uint32_t object, const Property& property, bool value, uint32_t index) {
  const uint32_t address = object + property.offset + index * property.element_size;
  const uint32_t word = GetU32(address);
  SetU32(address, value ? word | property.extra : word & ~property.extra);
}

}  // namespace cherry::engine::ue3
