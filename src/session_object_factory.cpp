// MIT License © 2025 Binary Dice Games
/// @file session_object_factory.cpp
/// @brief Implementation of the shared on_create_object helpers.
#include <session_object_factory.hpp>

#include <file_service.hpp>
#include <forms/form.hpp>
#include <logger.hpp>
#include <style_service.hpp>

#include "template_handler.hpp"

namespace bdg::wish::detail {

using namespace bison;

bison::dynamic_ptr find_singleton_service(const session& s, bison::key_t klass) {
  if (klass == "__WishFileSystem"_key && s.file_service)
    return dynamic_ptr{std::static_pointer_cast<dynamic>(s.file_service)};
  if (klass == "__WishStyle"_key && s.style_service)
    return dynamic_ptr{std::static_pointer_cast<dynamic>(s.style_service)};
  if (klass == "__WishLogger"_key && s.logger_service)
    return dynamic_ptr{std::static_pointer_cast<dynamic>(s.logger_service)};
  // NOTE: dynamic_ptr{} is NOT a null pointer -- dynamic_ptr's
  // (key_t klass = 0U, ...) constructor shadows shared_ptr's null default,
  // so it would build a real (but methodless) object here instead.  Use the
  // base shared_ptr's null constructor explicitly to signal "no match".
  return dynamic_ptr{std::shared_ptr<dynamic>{}};
}

void init_session_object(const bison::dynamic_ptr& obj, bison::rmi::context& ctx, const sync_session_ptr& sync_sess) {
  if (!obj || !sync_sess)
    return;
  if (auto* h = dynamic_cast<template_handler*>(obj.get())) {
    h->init(ctx, sync_sess);
  } else if (auto* f = dynamic_cast<form*>(obj.get())) {
    f->init(ctx, sync_sess);
  }
}

} // namespace bdg::wish::detail
