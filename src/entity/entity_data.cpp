#include "redstone/entity/entity_data.hpp"
#include <stdexcept>

namespace redstone {

void SynchedEntityData::defineRaw(std::uint8_t id,
                                  EntityDataSerializer serializer,
                                  EntityDataValue initial) {
  for (const auto &existing : items_) {
    if (existing.id == id)
      throw std::invalid_argument("duplicate SynchedEntityData id");
  }
  if (id > 254)
    throw std::invalid_argument("SynchedEntityData id exceeds 254");
  Item item;
  item.id = id;
  item.serializer = serializer;
  item.initialValue = initial;
  item.value = std::move(initial);
  item.dirty = false;
  items_.push_back(std::move(item));
}

bool SynchedEntityData::has(std::uint8_t id) const {
  for (const auto &item : items_)
    if (item.id == id)
      return true;
  return false;
}

const SynchedEntityData::Item &
SynchedEntityData::getItem(std::uint8_t id) const {
  for (const auto &item : items_)
    if (item.id == id)
      return item;
  throw std::out_of_range("missing SynchedEntityData id");
}

SynchedEntityData::Item &SynchedEntityData::getItem(std::uint8_t id) {
  for (auto &item : items_)
    if (item.id == id)
      return item;
  throw std::out_of_range("missing SynchedEntityData id");
}

void SynchedEntityData::setRaw(std::uint8_t id, EntityDataValue newValue,
                               bool forceDirty) {
  Item &item = getItem(id);
  if (item.value.index() != newValue.index())
    throw std::invalid_argument("mismatched SynchedEntityData value type");
  if (forceDirty || item.value != newValue) {
    item.value = std::move(newValue);
    if (onUpdate_)
      onUpdate_(id);
    item.dirty = true;
    item.changedRevision = ++revision_;
    dirty_ = true;
  }
}

std::vector<EntityDataEntry>
SynchedEntityData::dirtySince(std::uint64_t sinceRevision) const {
  if (revision_ <= sinceRevision)
    return {};
  std::vector<EntityDataEntry> result;
  for (const auto &item : items_) {
    if (item.changedRevision > sinceRevision)
      result.push_back(item.packed());
  }
  return result;
}

std::vector<EntityDataEntry> SynchedEntityData::packDirty() {
  if (!dirty_)
    return {};
  dirty_ = false;
  std::vector<EntityDataEntry> result;
  for (auto &item : items_) {
    if (item.dirty) {
      item.dirty = false;
      result.push_back(item.packed());
    }
  }
  return result;
}

std::vector<EntityDataEntry> SynchedEntityData::getNonDefaultValues() const {
  std::vector<EntityDataEntry> result;
  for (const auto &item : items_) {
    if (!item.isSetToDefault())
      result.push_back(item.packed());
  }
  return result;
}

std::vector<EntityDataEntry> SynchedEntityData::getAllValues() const {
  std::vector<EntityDataEntry> result;
  result.reserve(items_.size());
  for (const auto &item : items_)
    result.push_back(item.packed());
  return result;
}

} // namespace redstone
