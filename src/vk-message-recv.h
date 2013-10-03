// Receiving messages, marking messages as read.

#pragma once

#include "common.h"

#include <connection.h>

// Callback called when messages are received. max_msg_id is the max id of received messages if any have been
// received, zero otherwise.
using ReceivedCb = std::function<void(uint64 max_msg_id)>;

// Receives all messages starting after from_msg_id. If last_msg_id is zero only unread incoming messages
// are received, otherwise all messages (both sent and received) since last_msg_id are received, not including
// last_msg_id.
void receive_messages_range(PurpleConnection* gc, uint64 last_msg_id, const ReceivedCb& received_cb);

// Receives messages with given ids.
void receive_messages(PurpleConnection* gc, const uint64_vec& message_ids, const ReceivedCb& received_cb = nullptr);

// Marks messages as read.
void mark_message_as_read(PurpleConnection* gc, const uint64_vec& message_ids);
