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

#include <AK/TemporaryChange.h>
#include <AK/QuickSort.h>
#include <LibGUI/AbstractView.h>
#include <LibGUI/SortingProxyModel.h>
#include <stdio.h>
#include <stdlib.h>

namespace GUI {

SortingProxyModel::SortingProxyModel(NonnullRefPtr<Model>&& target)
    : m_target(move(target))
    , m_key_column(-1)
{
    // Since the target model already called Model::did_update we can't
    // assume we will get another call. So, we need to register for further
    // updates and just call resort() right away, otherwise requests
    // to this model won't work because there are no indices to map
    m_target->register_client(*this);
    resort();
}

SortingProxyModel::~SortingProxyModel()
{
    m_target->unregister_client(*this);
}

void SortingProxyModel::on_model_update(unsigned flags)
{
    resort(flags);
}

int SortingProxyModel::row_count(const ModelIndex& index) const
{
    return target().row_count(index);
}

int SortingProxyModel::column_count(const ModelIndex& index) const
{
    return target().column_count(index);
}

ModelIndex SortingProxyModel::map_to_target(const ModelIndex& index) const
{
    if (!index.is_valid())
        return {};
    if (static_cast<size_t>(index.row()) >= m_row_mappings.size() || index.column() >= column_count())
        return {};
    return target().index(m_row_mappings[index.row()], index.column());
}

String SortingProxyModel::column_name(int index) const
{
    return target().column_name(index);
}

Variant SortingProxyModel::data(const ModelIndex& index, Role role) const
{
    auto target_index = map_to_target(index);
    if (!target_index.is_valid()) {
        dbg() << "BUG! SortingProxyModel: Unable to convert " << index << " to target";
        return {};
    }
    return target().data(map_to_target(index), role);
}

void SortingProxyModel::update()
{
    target().update();
}

StringView SortingProxyModel::drag_data_type() const
{
    return target().drag_data_type();
}

void SortingProxyModel::set_key_column_and_sort_order(int column, SortOrder sort_order)
{
    if (column == m_key_column && sort_order == m_sort_order)
        return;

    ASSERT(column >= 0 && column < column_count());
    m_key_column = column;
    m_sort_order = sort_order;
    resort();
}

void SortingProxyModel::resort(unsigned flags)
{
    TemporaryChange change(m_sorting, true);
    auto old_row_mappings = m_row_mappings;
    int row_count = target().row_count();
    m_row_mappings.resize(row_count);
    for (int i = 0; i < row_count; ++i)
        m_row_mappings[i] = i;
    if (m_key_column == -1) {
        did_update(flags);
        return;
    }
    quick_sort(m_row_mappings, [&](auto row1, auto row2) -> bool {
        auto data1 = target().data(target().index(row1, m_key_column), m_sort_role);
        auto data2 = target().data(target().index(row2, m_key_column), m_sort_role);
        if (data1 == data2)
            return 0;
        bool is_less_than;
        if (data1.is_string() && data2.is_string() && !m_sorting_case_sensitive)
            is_less_than = data1.as_string().to_lowercase() < data2.as_string().to_lowercase();
        else
            is_less_than = data1 < data2;
        return m_sort_order == SortOrder::Ascending ? is_less_than : !is_less_than;
    });
    for_each_view([&](AbstractView& view) {
        view.selection().change_from_model({}, [&](ModelSelection& selection) {
            Vector<ModelIndex> selected_indexes_in_target;
            selection.for_each_index([&](const ModelIndex& index) {
                selected_indexes_in_target.append(target().index(old_row_mappings[index.row()], index.column()));
            });

            selection.clear();
            for (auto& index : selected_indexes_in_target) {
                for (size_t i = 0; i < m_row_mappings.size(); ++i) {
                    if (m_row_mappings[i] == index.row()) {
                        selection.add(this->index(i, index.column()));
                        continue;
                    }
                }
            }
        });
    });
    did_update(flags);
}

bool SortingProxyModel::is_column_sortable(int column_index) const
{
    return target().is_column_sortable(column_index);
}

}
