/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/MutationRecord.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeList.h>

namespace Web::DOM {

JS::NonnullGCPtr<MutationRecord> MutationRecord::create(JS::Realm& realm, DeprecatedFlyString const& type, Node& target, NodeList& added_nodes, NodeList& removed_nodes, Node* previous_sibling, Node* next_sibling, DeprecatedString const& attribute_name, DeprecatedString const& attribute_namespace, DeprecatedString const& old_value)
{
    return realm.heap().allocate<MutationRecord>(realm, realm, type, target, added_nodes, removed_nodes, previous_sibling, next_sibling, attribute_name, attribute_namespace, old_value).release_allocated_value_but_fixme_should_propagate_errors();
}

MutationRecord::MutationRecord(JS::Realm& realm, DeprecatedFlyString const& type, Node& target, NodeList& added_nodes, NodeList& removed_nodes, Node* previous_sibling, Node* next_sibling, DeprecatedString const& attribute_name, DeprecatedString const& attribute_namespace, DeprecatedString const& old_value)
    : PlatformObject(realm)
    , m_type(type)
    , m_target(JS::make_handle(target))
    , m_added_nodes(added_nodes)
    , m_removed_nodes(removed_nodes)
    , m_previous_sibling(JS::make_handle(previous_sibling))
    , m_next_sibling(JS::make_handle(next_sibling))
    , m_attribute_name(attribute_name)
    , m_attribute_namespace(attribute_namespace)
    , m_old_value(old_value)
{
}

MutationRecord::~MutationRecord() = default;

JS::ThrowCompletionOr<void> MutationRecord::initialize(JS::Realm& realm)
{
    MUST_OR_THROW_OOM(Base::initialize(realm));
    set_prototype(&Bindings::ensure_web_prototype<Bindings::MutationRecordPrototype>(realm, "MutationRecord"));

    return {};
}

void MutationRecord::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target.ptr());
    visitor.visit(m_added_nodes.ptr());
    visitor.visit(m_removed_nodes.ptr());
    visitor.visit(m_previous_sibling.ptr());
    visitor.visit(m_next_sibling.ptr());
}

}
