// Read-mostly access to the guest's UE3 object system: GObjObjects, FNames and script properties. Call only from the game thread.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cherry::engine::ue3 {

// UClass::ClassCastFlags bits.
constexpr uint32_t kCastProperty = 0x8000;
constexpr uint32_t kCastBoolProperty = 0x20000;
constexpr uint32_t kCastStructProperty = 0x100000;
constexpr uint32_t kCastArrayProperty = 0x200000;

// An FName as text, with the _N suffix its number adds.
std::string NameString(uint32_t index, uint32_t number);

uint32_t NameCount();
uint32_t ObjectCount();
uint32_t ObjectAt(uint32_t index);
uint32_t ObjectIndex(uint32_t object);
uint32_t ObjectClass(uint32_t object);
uint32_t ObjectOuter(uint32_t object);
std::string ObjectName(uint32_t object);
std::string ObjectPath(uint32_t object);
bool IsDefaultObject(uint32_t object);
bool IsA(uint32_t object, uint32_t cls);

// True while object is still registered in GObjObjects at its own index.
bool IsLive(uint32_t object);

uint32_t ClassCastFlags(uint32_t cls);
uint32_t SuperStruct(uint32_t structure);
uint32_t FirstChild(uint32_t structure);
uint32_t NextField(uint32_t field);

// A function's or state's compiled UnrealScript bytecode.
std::vector<uint8_t> Script(uint32_t structure);

// The class object called name, 0 when it is not loaded.
uint32_t FindClass(std::string_view name);

// Every live instance of cls or a subclass, class default objects excluded.
std::vector<uint32_t> Instances(uint32_t cls);

// A UProperty and the fields every reader needs; extra is BitMask for a bool, Inner for an array, Struct for a struct, Enum for a byte, PropertyClass for an object.
struct Property {
  uint32_t object = 0;
  uint32_t offset = 0;
  uint32_t array_dim = 0;
  uint32_t element_size = 0;
  uint32_t cast_flags = 0;
  uint32_t extra = 0;
  explicit operator bool() const { return object != 0; }
};

Property ReadProperty(uint32_t property);

// The field (property, function, state, struct...) called name on structure or one of its supers, 0 when there is none.
uint32_t FindField(uint32_t structure, std::string_view name);

// The property called name on structure or one of its supers.
Property FindProperty(uint32_t structure, std::string_view name);

bool GetBool(uint32_t object, const Property& property, uint32_t index = 0);
void SetBool(uint32_t object, const Property& property, bool value, uint32_t index = 0);
uint32_t GetU32(uint32_t address);
void SetU32(uint32_t address, uint32_t value);
uint8_t GetU8(uint32_t address);
void SetU8(uint32_t address, uint8_t value);

}  // namespace cherry::engine::ue3
