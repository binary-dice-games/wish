// MIT License © 2025 Binary Dice Games
/// @file server.cpp
/// @brief Implementation of wish::server.
#include <wish/server.hpp>
#include <wish/file_service.hpp>
#include <wish/logger.hpp>
#include <wish/style_service.hpp>
#include <wish/registry.hpp>

#include "template_handler.hpp"

#include <chrono>
#include <memory>
#include <thread>

namespace bdg::wish {

using namespace bison::rmi::transport;

// ── server ────────────────────────────────────────────────────────────────────

server::server(
    server_transport_iface& transport,
    std::unique_ptr<renderer> r)
    : bison::rmi::server(transport), renderer_(std::move(r)) {}

server::~server() {
  if (running_.load(std::memory_order_acquire)) {
    try {
      stop();
    } catch (...) {}
  }
}

void server::start() {
  register_all();
  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread([this]() { render_loop(); });
  listen();
}

void server::stop() {
  running_.store(false, std::memory_order_release);
  bison::rmi::server::stop();
  if (render_thread_.joinable()) {
    render_thread_.join();
  }
}

bool server::should_quit() const {
  return !running_.load(std::memory_order_acquire);
}

void server::on_session_created(bison::rmi::context& ctx) {
  auto sess = std::make_shared<session>(ctx.session_id);
  sess->emit_event = ctx.emit_event;
  sess->file_service = std::make_shared<file_service>(
      bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishFileSystem"}),
      sess->resource_dir);
  sess->style_service = std::make_shared<style_service>(
      bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishStyle"}));
  // All sessions share the same global logger instance (set via set_logger()).
  sess->logger_service = logger_;
  sessions_.wlock()->emplace(ctx.session_id.id, sess);
  on_session_created(*sess);
}

void server::on_session_destroyed(bison::rmi::context& ctx) {
  std::shared_ptr<session> sess;
  {
    auto lp = sessions_.wlock();
    auto it = lp->find(ctx.session_id.id);
    if (it != lp->end()) {
      sess = it->second;
      lp->erase(it);
    }
  }
  if (sess) {
    try {
      on_session_destroyed(*sess);
    } catch (...) {}
  }
}

bison::dynamic_ptr server::on_create_object(
    bison::rmi::context& ctx,
    bison::key_t ns,
    bison::key_t klass) {
  using namespace bison;

  std::shared_ptr<session> sess;
  {
    auto lp = sessions_.rlock();
    auto it = lp->find(ctx.session_id.id);
    if (it != lp->end()) {
      sess = it->second;
    }
  }

  // __WishFileSystem is a per-session singleton — return the pre-created instance.
  if (klass == "__WishFileSystem"_key && sess && sess->file_service) {
    return dynamic_ptr{std::static_pointer_cast<dynamic>(sess->file_service)};
  }

  // __WishStyle is a per-session singleton — return the pre-created instance.
  if (klass == "__WishStyle"_key && sess && sess->style_service) {
    return dynamic_ptr{std::static_pointer_cast<dynamic>(sess->style_service)};
  }

  // __WishLogger is a per-session singleton — return the pre-created instance.
  if (klass == "__WishLogger"_key && sess && sess->logger_service) {
    return dynamic_ptr{std::static_pointer_cast<dynamic>(sess->logger_service)};
  }

  // For all other classes, bison creates the concrete type from the registered
  // prototype.  Inject session context into template_handler instances.
  auto obj = bison::rmi::server::on_create_object(ctx, ns, klass);
  if (obj && sess) {
    if (auto* h = dynamic_cast<template_handler*>(obj.get())) {
      h->init(ctx, sess);
    }
  }
  return obj;
}

void server::render_loop() {
  if (renderer_) renderer_->setup();
  while (running_.load(std::memory_order_acquire)) {
    if (renderer_) {
      renderer_->begin_frame();
      renderer_->render_server_frame();
      {
        auto lp = sessions_.rlock();
        for (const auto& [id, sess] : *lp) {
          auto it = sess->objects.find("");
          if (it != sess->objects.end()) {
            renderer_->render_session(*it->second, *sess);
          }
        }
      }
      renderer_->end_frame();
      if (renderer_->should_quit())
        running_.store(false, std::memory_order_release);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  if (renderer_) renderer_->teardown();
}

} // namespace bdg::wish
