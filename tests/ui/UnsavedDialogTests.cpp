#include "ui/UnsavedDialog.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

sawer::Vec2d center(const sawer::UiRect bounds)
{
    return {
        bounds.x + bounds.width * 0.5,
        bounds.y + bounds.height * 0.5,
    };
}

} // namespace

TEST_CASE("unsaved dialog stays centered and reachable at narrow high DPI")
{
    sawer::UnsavedDialog dialog;
    dialog.open(480.0, 420.0, 2.0, "A long Unicode board \xE2\x9C\xA8");

    REQUIRE(dialog.visible());
    REQUIRE(dialog.board_name() == "A long Unicode board \xE2\x9C\xA8");
    const sawer::UiRect panel = dialog.bounds();
    REQUIRE(panel.x >= 0.0);
    REQUIRE(panel.y >= 0.0);
    REQUIRE(panel.x + panel.width <= 480.0);
    REQUIRE(panel.y + panel.height <= 420.0);

    const auto cancel =
        dialog.button_bounds(sawer::UnsavedDialogChoice::cancel);
    const auto discard =
        dialog.button_bounds(sawer::UnsavedDialogChoice::discard);
    const auto save =
        dialog.button_bounds(sawer::UnsavedDialogChoice::save);
    REQUIRE(cancel.width >= 32.0);
    REQUIRE(cancel.height >= 32.0);
    REQUIRE(cancel.x + cancel.width < discard.x);
    REQUIRE(discard.x + discard.width < save.x);
    REQUIRE(save.x + save.width <= panel.x + panel.width);
}

TEST_CASE("unsaved dialog isolates clicks and supports safe keyboard choices")
{
    sawer::UnsavedDialog dialog;
    dialog.open(1280.0, 720.0, 1.0, "Untitled");

    REQUIRE(
        dialog.focused_choice() == sawer::UnsavedDialogChoice::save);
    dialog.focus_next();
    REQUIRE(
        dialog.focused_choice() == sawer::UnsavedDialogChoice::cancel);
    dialog.focus_next(true);
    REQUIRE(
        dialog.focused_choice() == sawer::UnsavedDialogChoice::save);

    const auto discard =
        dialog.button_bounds(sawer::UnsavedDialogChoice::discard);
    dialog.pointer_down(center(discard));
    REQUIRE_FALSE(dialog.pointer_up({discard.x - 10.0, discard.y}));

    dialog.pointer_down(center(discard));
    REQUIRE(
        dialog.pointer_up(center(discard))
        == sawer::UnsavedDialogChoice::discard);

    dialog.set_pointer(center(discard));
    dialog.tick(0.1);
    REQUIRE(dialog.hover(sawer::UnsavedDialogChoice::discard) > 0.5);
    REQUIRE(dialog.reveal() > 0.5);

    dialog.close();
    REQUIRE_FALSE(dialog.visible());
}
