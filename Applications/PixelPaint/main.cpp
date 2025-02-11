/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "CreateNewLayerDialog.h"
#include "Image.h"
#include "ImageEditor.h"
#include "Layer.h"
#include "LayerListWidget.h"
#include "PaletteWidget.h"
#include "Tool.h"
#include "ToolboxWidget.h"
#include <LibGUI/AboutDialog.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MenuBar.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/TableView.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    if (pledge("stdio thread shared_buffer accept rpath unix wpath cpath fattr", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    auto app = GUI::Application::construct(argc, argv);

    if (pledge("stdio thread shared_buffer accept rpath wpath cpath", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    auto app_icon = GUI::Icon::default_icon("app-pixel-paint");

    auto window = GUI::Window::construct();
    window->set_title("PixelPaint");
    window->set_rect(40, 100, 950, 570);
    window->set_icon(app_icon.bitmap_for_size(16));

    auto& horizontal_container = window->set_main_widget<GUI::Widget>();
    horizontal_container.set_layout<GUI::HorizontalBoxLayout>();
    horizontal_container.layout()->set_spacing(0);

    auto& toolbox = horizontal_container.add<PixelPaint::ToolboxWidget>();

    auto& vertical_container = horizontal_container.add<GUI::Widget>();
    vertical_container.set_layout<GUI::VerticalBoxLayout>();
    vertical_container.layout()->set_spacing(0);

    auto& image_editor = vertical_container.add<PixelPaint::ImageEditor>();
    image_editor.set_focus(true);

    toolbox.on_tool_selection = [&](auto* tool) {
        image_editor.set_active_tool(tool);
    };

    vertical_container.add<PixelPaint::PaletteWidget>(image_editor);

    auto& right_panel = horizontal_container.add<GUI::Widget>();
    right_panel.set_fill_with_background_color(true);
    right_panel.set_size_policy(GUI::SizePolicy::Fixed, GUI::SizePolicy::Fill);
    right_panel.set_preferred_size(230, 0);
    right_panel.set_layout<GUI::VerticalBoxLayout>();

    auto& layer_list_widget = right_panel.add<PixelPaint::LayerListWidget>();

    window->show();

    auto menubar = GUI::MenuBar::construct();
    auto& app_menu = menubar->add_menu("PixelPaint");

    app_menu.add_action(GUI::CommonActions::make_open_action([&](auto&) {
        Optional<String> open_path = GUI::FilePicker::get_open_filepath();

        if (!open_path.has_value())
            return;

        auto bitmap = Gfx::Bitmap::load_from_file(open_path.value());
        if (!bitmap) {
            GUI::MessageBox::show(String::format("Failed to load '%s'", open_path.value().characters()), "Open failed", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, window);
            return;
        }
    }));
    app_menu.add_separator();
    app_menu.add_action(GUI::CommonActions::make_quit_action([](auto&) {
        GUI::Application::the()->quit();
        return;
    }));

    menubar->add_menu("Edit");

    auto& tool_menu = menubar->add_menu("Tool");
    toolbox.for_each_tool([&](auto& tool) {
        if (tool.action())
            tool_menu.add_action(*tool.action());
        return IterationDecision::Continue;
    });

    auto& layer_menu = menubar->add_menu("Layer");
    layer_menu.add_action(GUI::Action::create(
        "Create new layer...", { Mod_Ctrl | Mod_Shift, Key_N }, [&](auto&) {
            auto dialog = PixelPaint::CreateNewLayerDialog::construct(image_editor.image()->size(), window);
            if (dialog->exec() == GUI::Dialog::ExecOK) {
                auto layer = PixelPaint::Layer::create_with_size(dialog->layer_size(), dialog->layer_name());
                if (!layer) {
                    GUI::MessageBox::show_error(String::format("Unable to create layer with size %s", dialog->size().to_string().characters()));
                    return;
                }
                image_editor.image()->add_layer(layer.release_nonnull());
                image_editor.layers_did_change();
            }
        },
        window));

    layer_menu.add_separator();
    layer_menu.add_action(GUI::Action::create(
        "Select previous layer", { 0, Key_PageUp }, [&](auto&) {
            layer_list_widget.move_selection(1);
        },
        window));
    layer_menu.add_action(GUI::Action::create(
        "Select next layer", { 0, Key_PageDown }, [&](auto&) {
            layer_list_widget.move_selection(-1);
        },
        window));
    layer_menu.add_action(GUI::Action::create(
        "Select top layer", { 0, Key_Home }, [&](auto&) {
            layer_list_widget.select_top_layer();
        },
        window));
    layer_menu.add_action(GUI::Action::create(
        "Select bottom layer", { 0, Key_End }, [&](auto&) {
            layer_list_widget.select_bottom_layer();
        },
        window));
    layer_menu.add_separator();
    layer_menu.add_action(GUI::Action::create(
        "Move active layer up", { Mod_Ctrl, Key_PageUp }, [&](auto&) {
            auto active_layer = image_editor.active_layer();
            if (!active_layer)
                return;
            image_editor.image()->move_layer_up(*active_layer);
        },
        window));
    layer_menu.add_action(GUI::Action::create(
        "Move active layer down", { Mod_Ctrl, Key_PageDown }, [&](auto&) {
            auto active_layer = image_editor.active_layer();
            if (!active_layer)
                return;
            image_editor.image()->move_layer_down(*active_layer);
        },
        window));
    layer_menu.add_separator();
    layer_menu.add_action(GUI::Action::create(
        "Remove active layer", { Mod_Ctrl, Key_D }, [&](auto&) {
            auto active_layer = image_editor.active_layer();
            if (!active_layer)
                return;
            image_editor.image()->remove_layer(*active_layer);
            image_editor.set_active_layer(nullptr);
        },
        window));

    auto& help_menu = menubar->add_menu("Help");
    help_menu.add_action(GUI::Action::create("About", [&](auto&) {
        GUI::AboutDialog::show("PixelPaint", app_icon.bitmap_for_size(32), window);
    }));

    app->set_menubar(move(menubar));

    image_editor.on_active_layer_change = [&](auto* layer) {
        layer_list_widget.set_selected_layer(layer);
    };

    auto image = PixelPaint::Image::create_with_size({ 640, 480 });

    auto bg_layer = PixelPaint::Layer::create_with_size({ 640, 480 }, "Background");
    image->add_layer(*bg_layer);
    bg_layer->bitmap().fill(Color::White);

    auto fg_layer1 = PixelPaint::Layer::create_with_size({ 200, 200 }, "FG Layer 1");
    fg_layer1->set_location({ 50, 50 });
    image->add_layer(*fg_layer1);
    fg_layer1->bitmap().fill(Color::Yellow);

    auto fg_layer2 = PixelPaint::Layer::create_with_size({ 100, 100 }, "FG Layer 2");
    fg_layer2->set_location({ 300, 300 });
    image->add_layer(*fg_layer2);
    fg_layer2->bitmap().fill(Color::Blue);

    layer_list_widget.on_layer_select = [&](auto* layer) {
        image_editor.set_active_layer(layer);
    };

    layer_list_widget.set_image(image);

    image_editor.set_image(image);
    image_editor.set_active_layer(bg_layer);

    return app->exec();
}
