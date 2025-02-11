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

#include <LibGUI/Painter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Frame/Frame.h>
#include <LibWeb/Layout/LayoutBlock.h>
#include <LibWeb/Layout/LayoutDocument.h>
#include <LibWeb/Layout/LayoutNode.h>
#include <LibWeb/Layout/LayoutReplaced.h>

namespace Web {

LayoutNode::LayoutNode(Document& document, const Node* node)
    : m_document(document)
    , m_node(node)
{
    if (m_node)
        m_node->set_layout_node({}, this);
}

LayoutNode::~LayoutNode()
{
    if (m_node && m_node->layout_node() == this)
        m_node->set_layout_node({}, nullptr);
}

void LayoutNode::layout(LayoutMode layout_mode)
{
    for_each_child([&](auto& child) {
        child.layout(layout_mode);
    });
}

bool LayoutNode::can_contain_boxes_with_position_absolute() const
{
    return style().position() != CSS::Position::Static || is_root();
}

const LayoutBlock* LayoutNode::containing_block() const
{
    auto nearest_block_ancestor = [this] {
        auto* ancestor = parent();
        while (ancestor && !is<LayoutBlock>(*ancestor))
            ancestor = ancestor->parent();
        return to<LayoutBlock>(ancestor);
    };

    if (is_text())
        return nearest_block_ancestor();

    auto position = style().position();

    if (position == CSS::Position::Absolute) {
        auto* ancestor = parent();
        while (ancestor && !ancestor->can_contain_boxes_with_position_absolute())
            ancestor = ancestor->parent();
        while (ancestor && (!is<LayoutBlock>(ancestor) || ancestor->is_anonymous()))
            ancestor = ancestor->containing_block();
        return to<LayoutBlock>(ancestor);
    }

    if (position == CSS::Position::Fixed)
        return &root();

    return nearest_block_ancestor();
}

void LayoutNode::paint(PaintContext& context, PaintPhase phase)
{
    if (!is_visible())
        return;

    for_each_child([&](auto& child) {
        if (child.is_box() && to<LayoutBox>(child).stacking_context())
            return;
        child.paint(context, phase);
    });
}

HitTestResult LayoutNode::hit_test(const Gfx::IntPoint& position) const
{
    HitTestResult result;
    for_each_child([&](auto& child) {
        // Skip over children that establish their own stacking context.
        // The outer loop who called us will take care of those.
        if (is<LayoutBox>(child) && to<LayoutBox>(child).stacking_context())
            return;
        auto child_result = child.hit_test(position);
        if (child_result.layout_node)
            result = child_result;
    });
    return result;
}

const Frame& LayoutNode::frame() const
{
    ASSERT(document().frame());
    return *document().frame();
}

Frame& LayoutNode::frame()
{
    ASSERT(document().frame());
    return *document().frame();
}

const LayoutDocument& LayoutNode::root() const
{
    ASSERT(document().layout_node());
    return *document().layout_node();
}

LayoutDocument& LayoutNode::root()
{
    ASSERT(document().layout_node());
    return *document().layout_node();
}

void LayoutNode::split_into_lines(LayoutBlock& container, LayoutMode layout_mode)
{
    for_each_child([&](auto& child) {
        child.split_into_lines(container, layout_mode);
    });
}

void LayoutNode::set_needs_display()
{
    if (auto* block = containing_block()) {
        block->for_each_fragment([&](auto& fragment) {
            if (&fragment.layout_node() == this || is_ancestor_of(fragment.layout_node())) {
                frame().set_needs_display(enclosing_int_rect(fragment.absolute_rect()));
            }
            return IterationDecision::Continue;
        });
    }
}

float LayoutNode::font_size() const
{
    // FIXME: This doesn't work right for relative font-sizes
    auto length = specified_style().length_or_fallback(CSS::PropertyID::FontSize, Length(10, Length::Type::Px));
    return length.raw_value();
}

Gfx::FloatPoint LayoutNode::box_type_agnostic_position() const
{
    if (is_box())
        return to<LayoutBox>(*this).absolute_position();
    ASSERT(is_inline());
    Gfx::FloatPoint position;
    if (auto* block = containing_block()) {
        block->for_each_fragment([&](auto& fragment) {
            if (&fragment.layout_node() == this || is_ancestor_of(fragment.layout_node())) {
                position = fragment.absolute_rect().location();
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });
    }
    return position;
}

bool LayoutNode::is_floating() const
{
    if (!has_style())
        return false;
    return style().float_() != CSS::Float::None;
}

bool LayoutNode::is_absolutely_positioned() const
{
    if (!has_style())
        return false;
    auto position = style().position();
    return position == CSS::Position::Absolute || position == CSS::Position::Fixed;
}

bool LayoutNode::is_fixed_position() const
{
    if (!has_style())
        return false;
    auto position = style().position();
    return position == CSS::Position::Fixed;
}

LayoutNodeWithStyle::LayoutNodeWithStyle(Document& document, const Node* node, NonnullRefPtr<StyleProperties> specified_style)
    : LayoutNode(document, node)
    , m_specified_style(move(specified_style))
{
    m_has_style = true;
    apply_style(*m_specified_style);
}

void LayoutNodeWithStyle::apply_style(const StyleProperties& specified_style)
{
    auto& style = static_cast<MutableLayoutStyle&>(m_style);

    style.set_position(specified_style.position());
    style.set_text_align(specified_style.text_align());

    auto white_space = specified_style.white_space();
    if (white_space.has_value())
        style.set_white_space(white_space.value());

    auto float_ = specified_style.float_();
    if (float_.has_value())
        style.set_float(float_.value());

    style.set_z_index(specified_style.z_index());
    style.set_width(specified_style.length_or_fallback(CSS::PropertyID::Width, {}));
    style.set_min_width(specified_style.length_or_fallback(CSS::PropertyID::MinWidth, {}));
    style.set_max_width(specified_style.length_or_fallback(CSS::PropertyID::MaxWidth, {}));
    style.set_height(specified_style.length_or_fallback(CSS::PropertyID::Height, {}));
    style.set_min_height(specified_style.length_or_fallback(CSS::PropertyID::MinHeight, {}));
    style.set_max_height(specified_style.length_or_fallback(CSS::PropertyID::MaxHeight, {}));

    style.set_offset(specified_style.length_box(CSS::PropertyID::Left, CSS::PropertyID::Top, CSS::PropertyID::Right, CSS::PropertyID::Bottom));
    style.set_margin(specified_style.length_box(CSS::PropertyID::MarginLeft, CSS::PropertyID::MarginTop, CSS::PropertyID::MarginRight, CSS::PropertyID::MarginBottom));
    style.set_padding(specified_style.length_box(CSS::PropertyID::PaddingLeft, CSS::PropertyID::PaddingTop, CSS::PropertyID::PaddingRight, CSS::PropertyID::PaddingBottom));

    style.border_left().width = specified_style.length_or_fallback(CSS::PropertyID::BorderLeftWidth, {}).resolved_or_zero(*this, 0).to_px(*this);
    style.border_top().width = specified_style.length_or_fallback(CSS::PropertyID::BorderTopWidth, {}).resolved_or_zero(*this, 0).to_px(*this);
    style.border_right().width = specified_style.length_or_fallback(CSS::PropertyID::BorderRightWidth, {}).resolved_or_zero(*this, 0).to_px(*this);
    style.border_bottom().width = specified_style.length_or_fallback(CSS::PropertyID::BorderBottomWidth, {}).resolved_or_zero(*this, 0).to_px(*this);

    style.border_left().color = specified_style.color_or_fallback(CSS::PropertyID::BorderLeftColor, document(), Color::Transparent);
    style.border_top().color = specified_style.color_or_fallback(CSS::PropertyID::BorderTopColor, document(), Color::Transparent);
    style.border_right().color = specified_style.color_or_fallback(CSS::PropertyID::BorderRightColor, document(), Color::Transparent);
    style.border_bottom().color = specified_style.color_or_fallback(CSS::PropertyID::BorderBottomColor, document(), Color::Transparent);
}

}
