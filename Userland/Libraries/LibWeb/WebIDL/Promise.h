/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/SafeFunction.h>
#include <LibWeb/Forward.h>

namespace Web::WebIDL {

using ReactionSteps = JS::SafeFunction<WebIDL::ExceptionOr<JS::Value>(JS::Value)>;

// https://webidl.spec.whatwg.org/#es-promise
using Promise = JS::PromiseCapability;

JS::NonnullGCPtr<Promise> create_promise(JS::Realm&);
JS::NonnullGCPtr<Promise> create_resolved_promise(JS::Realm&, JS::Value);
JS::NonnullGCPtr<Promise> create_rejected_promise(JS::Realm&, JS::Value);
void resolve_promise(JS::VM&, Promise const&, JS::Value = JS::js_undefined());
void reject_promise(JS::VM&, Promise const&, JS::Value);
JS::NonnullGCPtr<JS::Promise> react_to_promise(Promise const&, Optional<ReactionSteps> on_fulfilled_callback, Optional<ReactionSteps> on_rejected_callback);
JS::NonnullGCPtr<JS::Promise> upon_fulfillment(Promise const&, ReactionSteps);
JS::NonnullGCPtr<JS::Promise> upon_rejection(Promise const&, ReactionSteps);
void mark_promise_as_handled(Promise const&);

}
