#include "document/Command.hpp"

#include <stdexcept>
#include <utility>

namespace sawer {
namespace {

[[noreturn]] void command_error(const char* const message)
{
    throw std::logic_error{message};
}

std::size_t dynamic_object_bytes(const Object& object) noexcept
{
    const auto* const stroke = std::get_if<Stroke>(&object.geometry);
    return stroke != nullptr
        ? stroke->points.capacity() * sizeof(Vec2d)
        : 0U;
}

void collect_object_asset(
    const Object& object,
    RetainedAssets& assets)
{
    const auto* const image = std::get_if<Image>(&object.geometry);
    if (image != nullptr && image->asset != nullptr) {
        assets.emplace(image->asset->id, image->asset);
    }
}

std::size_t retained_asset_bytes(const RetainedAssets& assets) noexcept
{
    std::size_t result{};
    for (const auto& [id, asset] : assets) {
        static_cast<void>(id);
        result += sizeof(ImageAsset) + asset->png.capacity()
            + asset->preview.capacity();
    }
    return result;
}

} // namespace

AddObjectCommand::AddObjectCommand(Object object)
    : id_{object.id}
    , detached_{std::move(object)}
{
}

void AddObjectCommand::apply(Document& document)
{
    if (!detached_.has_value() || document.find(id_) != nullptr
        || !document.insert(std::move(*detached_))) {
        command_error("Cannot add a duplicate object");
    }
    detached_.reset();
}

void AddObjectCommand::revert(Document& document)
{
    detached_ = document.remove(id_);
    if (!detached_.has_value()) {
        command_error("Cannot undo an object that is not present");
    }
}

std::vector<ObjectId> AddObjectCommand::object_ids() const
{
    return {id_};
}

std::size_t AddObjectCommand::retained_bytes() const noexcept
{
    return sizeof(*this)
        + (detached_.has_value()
            ? dynamic_object_bytes(*detached_)
            : 0U);
}

void AddObjectCommand::collect_retained_assets(RetainedAssets& assets) const
{
    if (detached_.has_value()) collect_object_asset(*detached_, assets);
}

DeleteObjectCommand::DeleteObjectCommand(const ObjectId id)
    : id_{id}
{
}

void DeleteObjectCommand::apply(Document& document)
{
    detached_ = document.remove(id_);
    if (!detached_.has_value()) {
        command_error("Cannot delete an object that is not present");
    }
}

void DeleteObjectCommand::revert(Document& document)
{
    if (!detached_.has_value() || document.find(id_) != nullptr
        || !document.insert(std::move(*detached_))) {
        command_error("Cannot restore a deleted object");
    }
    detached_.reset();
}

std::vector<ObjectId> DeleteObjectCommand::object_ids() const
{
    return {id_};
}

std::size_t DeleteObjectCommand::retained_bytes() const noexcept
{
    return sizeof(*this)
        + (detached_.has_value()
            ? dynamic_object_bytes(*detached_)
            : 0U);
}

void DeleteObjectCommand::collect_retained_assets(RetainedAssets& assets) const
{
    if (detached_.has_value()) collect_object_asset(*detached_, assets);
}

ModifyObjectCommand::ModifyObjectCommand(Object replacement)
    : alternate_{std::move(replacement)}
{
}

void ModifyObjectCommand::apply(Document& document)
{
    if (document.find(alternate_.id) == nullptr) {
        command_error("Cannot modify an object that is not present");
    }
    auto previous = document.exchange(std::move(alternate_));
    if (!previous.has_value()) {
        command_error("Cannot apply object modification");
    }
    alternate_ = std::move(*previous);
}

void ModifyObjectCommand::revert(Document& document)
{
    auto previous = document.exchange(std::move(alternate_));
    if (!previous.has_value()) {
        command_error("Cannot revert object modification");
    }
    alternate_ = std::move(*previous);
}

std::vector<ObjectId> ModifyObjectCommand::object_ids() const
{
    return {alternate_.id};
}

std::size_t ModifyObjectCommand::retained_bytes() const noexcept
{
    return sizeof(*this) + dynamic_object_bytes(alternate_);
}

void ModifyObjectCommand::collect_retained_assets(RetainedAssets& assets) const
{
    collect_object_asset(alternate_, assets);
}

ChangeStyleCommand::ChangeStyleCommand(
    const ObjectId id,
    Style replacement)
    : id_{id}
    , alternate_{std::move(replacement)}
{
}

void ChangeStyleCommand::apply(Document& document)
{
    exchange(document);
}

void ChangeStyleCommand::revert(Document& document)
{
    exchange(document);
}

std::vector<ObjectId> ChangeStyleCommand::object_ids() const
{
    return {id_};
}

std::size_t ChangeStyleCommand::retained_bytes() const noexcept
{
    return sizeof(*this);
}

void ChangeStyleCommand::collect_retained_assets(RetainedAssets&) const {}

void ChangeStyleCommand::exchange(Document& document)
{
    auto previous = document.exchange_style(id_, std::move(alternate_));
    if (!previous.has_value()) {
        command_error("Cannot change style of a missing object");
    }
    alternate_ = std::move(*previous);
}

MoveObjectCommand::MoveObjectCommand(
    const ObjectId id,
    const Vec2d delta)
    : id_{id}
    , delta_{delta}
{
}

void MoveObjectCommand::apply(Document& document)
{
    if (!document.translate(id_, delta_)) {
        command_error("Cannot move an object that is not present");
    }
}

void MoveObjectCommand::revert(Document& document)
{
    if (!document.translate(id_, {-delta_.x, -delta_.y})) {
        command_error("Cannot undo movement of a missing object");
    }
}

std::vector<ObjectId> MoveObjectCommand::object_ids() const
{
    return {id_};
}

std::size_t MoveObjectCommand::retained_bytes() const noexcept
{
    return sizeof(*this);
}

void MoveObjectCommand::collect_retained_assets(RetainedAssets&) const {}

CompositeCommand::CompositeCommand(
    std::vector<std::unique_ptr<Command>> commands)
    : commands_{std::move(commands)}
{
    if (commands_.empty()) {
        command_error("A composite command must contain an operation");
    }
}

void CompositeCommand::apply(Document& document)
{
    std::size_t applied = 0U;
    try {
        for (; applied < commands_.size(); ++applied) {
            commands_[applied]->apply(document);
        }
    } catch (...) {
        while (applied > 0U) {
            --applied;
            commands_[applied]->revert(document);
        }
        throw;
    }
}

void CompositeCommand::revert(Document& document)
{
    std::size_t reverted = 0U;
    try {
        for (; reverted < commands_.size(); ++reverted) {
            commands_[commands_.size() - 1U - reverted]->revert(document);
        }
    } catch (...) {
        while (reverted > 0U) {
            --reverted;
            commands_[commands_.size() - 1U - reverted]->apply(document);
        }
        throw;
    }
}

std::vector<ObjectId> CompositeCommand::object_ids() const
{
    std::vector<ObjectId> ids;
    ids.reserve(commands_.size());
    for (const auto& command : commands_) {
        const auto command_ids = command->object_ids();
        ids.insert(ids.end(), command_ids.begin(), command_ids.end());
    }
    return ids;
}

std::size_t CompositeCommand::retained_bytes() const noexcept
{
    std::size_t bytes = sizeof(*this)
        + commands_.capacity() * sizeof(std::unique_ptr<Command>);
    for (const auto& command : commands_) {
        bytes += command->retained_bytes();
    }
    return bytes;
}

void CompositeCommand::collect_retained_assets(RetainedAssets& assets) const
{
    for (const auto& command : commands_) {
        command->collect_retained_assets(assets);
    }
}

void CommandHistory::execute(
    std::unique_ptr<Command> command,
    Document& document)
{
    command->apply(document);

    if (cursor_ < commands_.size()) {
        commands_.erase(
            commands_.begin() + static_cast<std::ptrdiff_t>(cursor_),
            commands_.end());
        if (clean_cursor_.has_value() && *clean_cursor_ > cursor_) {
            clean_cursor_.reset();
        }
    }

    commands_.push_back(std::move(command));
    ++cursor_;
    prune();
    update_dirty(document);
}

std::vector<ObjectId> CommandHistory::undo(Document& document)
{
    if (!can_undo()) {
        return {};
    }
    Command& command = *commands_[cursor_ - 1U];
    command.revert(document);
    --cursor_;
    update_dirty(document);
    return command.object_ids();
}

std::vector<ObjectId> CommandHistory::redo(Document& document)
{
    if (!can_redo()) {
        return {};
    }
    Command& command = *commands_[cursor_];
    command.apply(document);
    ++cursor_;
    update_dirty(document);
    return command.object_ids();
}

void CommandHistory::mark_saved(Document& document) noexcept
{
    clean_cursor_ = cursor_;
    update_dirty(document);
}

bool CommandHistory::can_undo() const noexcept
{
    return cursor_ > 0U;
}

bool CommandHistory::can_redo() const noexcept
{
    return cursor_ < commands_.size();
}

std::size_t CommandHistory::size() const noexcept
{
    return commands_.size();
}

std::size_t CommandHistory::retained_bytes() const noexcept
{
    std::size_t bytes = sizeof(*this)
        + commands_.capacity() * sizeof(std::unique_ptr<Command>);
    for (const auto& command : commands_) {
        bytes += command->retained_bytes();
    }
    RetainedAssets assets;
    for (const auto& command : commands_) {
        command->collect_retained_assets(assets);
    }
    return bytes + retained_asset_bytes(assets);
}

void CommandHistory::prune()
{
    std::size_t remove_count = 0U;
    std::size_t bytes = retained_bytes();
    while (commands_.size() - remove_count > maximum_entries
           || (commands_.size() - remove_count > 1U
               && bytes > maximum_retained_bytes)) {
        ++remove_count;
        bytes = sizeof(*this)
            + commands_.capacity() * sizeof(std::unique_ptr<Command>);
        RetainedAssets assets;
        for (std::size_t index = remove_count; index < commands_.size(); ++index) {
            bytes += commands_[index]->retained_bytes();
            commands_[index]->collect_retained_assets(assets);
        }
        bytes += retained_asset_bytes(assets);
    }
    if (remove_count == 0U) {
        return;
    }

    commands_.erase(
        commands_.begin(),
        commands_.begin() + static_cast<std::ptrdiff_t>(remove_count));
    cursor_ -= std::min(cursor_, remove_count);
    if (clean_cursor_.has_value()) {
        if (*clean_cursor_ < remove_count) {
            clean_cursor_.reset();
        } else {
            *clean_cursor_ -= remove_count;
        }
    }
}

void CommandHistory::update_dirty(Document& document) const noexcept
{
    document.set_dirty(
        !clean_cursor_.has_value() || cursor_ != *clean_cursor_);
}

} // namespace sawer
