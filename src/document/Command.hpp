#pragma once

#include "document/Document.hpp"

#include <cstddef>
#include <memory>
#include <map>
#include <optional>
#include <vector>

namespace sawer {

using RetainedAssets = std::map<AssetId, std::shared_ptr<const ImageAsset>>;

class Command {
public:
    virtual ~Command() = default;
    virtual void apply(Document& document) = 0;
    virtual void revert(Document& document) = 0;
    [[nodiscard]] virtual std::vector<ObjectId> object_ids() const = 0;
    [[nodiscard]] virtual std::size_t retained_bytes() const noexcept = 0;
    virtual void collect_retained_assets(RetainedAssets& assets) const = 0;
};

class AddObjectCommand final : public Command {
public:
    explicit AddObjectCommand(Object object);
    void apply(Document& document) override;
    void revert(Document& document) override;
    [[nodiscard]] std::vector<ObjectId> object_ids() const override;
    [[nodiscard]] std::size_t retained_bytes() const noexcept override;
    void collect_retained_assets(RetainedAssets& assets) const override;

private:
    ObjectId id_;
    std::optional<Object> detached_;
};

class DeleteObjectCommand final : public Command {
public:
    explicit DeleteObjectCommand(ObjectId id);
    void apply(Document& document) override;
    void revert(Document& document) override;
    [[nodiscard]] std::vector<ObjectId> object_ids() const override;
    [[nodiscard]] std::size_t retained_bytes() const noexcept override;
    void collect_retained_assets(RetainedAssets& assets) const override;

private:
    ObjectId id_;
    std::optional<Object> detached_;
};

class ModifyObjectCommand final : public Command {
public:
    explicit ModifyObjectCommand(Object replacement);
    void apply(Document& document) override;
    void revert(Document& document) override;
    [[nodiscard]] std::vector<ObjectId> object_ids() const override;
    [[nodiscard]] std::size_t retained_bytes() const noexcept override;
    void collect_retained_assets(RetainedAssets& assets) const override;

private:
    Object alternate_;
};

class ChangeStyleCommand final : public Command {
public:
    ChangeStyleCommand(ObjectId id, Style replacement);
    void apply(Document& document) override;
    void revert(Document& document) override;
    [[nodiscard]] std::vector<ObjectId> object_ids() const override;
    [[nodiscard]] std::size_t retained_bytes() const noexcept override;
    void collect_retained_assets(RetainedAssets& assets) const override;

private:
    void exchange(Document& document);

    ObjectId id_;
    Style alternate_;
};

class MoveObjectCommand final : public Command {
public:
    MoveObjectCommand(ObjectId id, Vec2d delta);
    void apply(Document& document) override;
    void revert(Document& document) override;
    [[nodiscard]] std::vector<ObjectId> object_ids() const override;
    [[nodiscard]] std::size_t retained_bytes() const noexcept override;
    void collect_retained_assets(RetainedAssets& assets) const override;

private:
    ObjectId id_;
    Vec2d delta_;
};

class CompositeCommand final : public Command {
public:
    explicit CompositeCommand(std::vector<std::unique_ptr<Command>> commands);
    void apply(Document& document) override;
    void revert(Document& document) override;
    [[nodiscard]] std::vector<ObjectId> object_ids() const override;
    [[nodiscard]] std::size_t retained_bytes() const noexcept override;
    void collect_retained_assets(RetainedAssets& assets) const override;

private:
    std::vector<std::unique_ptr<Command>> commands_;
};

class CommandHistory final {
public:
    void execute(std::unique_ptr<Command> command, Document& document);
    [[nodiscard]] std::vector<ObjectId> undo(Document& document);
    [[nodiscard]] std::vector<ObjectId> redo(Document& document);
    void mark_saved(Document& document) noexcept;

    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t retained_bytes() const noexcept;

private:
    void prune();
    void update_dirty(Document& document) const noexcept;

    static constexpr std::size_t maximum_entries = 1'000U;
    static constexpr std::size_t maximum_retained_bytes =
        256U * 1'024U * 1'024U;

    std::vector<std::unique_ptr<Command>> commands_;
    std::size_t cursor_{};
    std::optional<std::size_t> clean_cursor_{0U};
};

} // namespace sawer
