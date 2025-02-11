/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
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

#include <LibCore/ElapsedTimer.h>
#include <LibGUI/Button.h>
#include <LibGUI/TextBox.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/HTMLFormElement.h>
#include <LibWeb/DOM/HTMLIFrameElement.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Frame/Frame.h>
#include <LibWeb/Layout/LayoutFrame.h>
#include <LibWeb/Layout/LayoutWidget.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/PageView.h>
#include <LibWeb/Parser/HTMLDocumentParser.h>

namespace Web {

HTMLIFrameElement::HTMLIFrameElement(Document& document, const FlyString& tag_name)
    : HTMLElement(document, tag_name)
{
}

HTMLIFrameElement::~HTMLIFrameElement()
{
}

RefPtr<LayoutNode> HTMLIFrameElement::create_layout_node(const StyleProperties* parent_style)
{
    auto style = document().style_resolver().resolve_style(*this, parent_style);
    return adopt(*new LayoutFrame(document(), *this, move(style)));
}

void HTMLIFrameElement::document_did_attach_to_frame(Frame& frame)
{
    ASSERT(!m_hosted_frame);
    m_hosted_frame = Frame::create_subframe(*this, frame.main_frame());
    auto src = attribute(HTML::AttributeNames::src);
    if (src.is_null())
        return;
    load_src(src);
}

void HTMLIFrameElement::document_will_detach_from_frame(Frame&)
{
}

void HTMLIFrameElement::load_src(const String& value)
{
    dbg() << "Loading iframe document from " << value;
    auto url = document().complete_url(value);
    if (!url.is_valid()) {
        dbg() << "Actually no I'm not, because the URL is not valid :(";
        return;
    }

    m_hosted_frame->loader().load(url, FrameLoader::Type::IFrame);
}

const Document* HTMLIFrameElement::hosted_document() const
{
    return m_hosted_frame ? m_hosted_frame->document() : nullptr;
}

}
