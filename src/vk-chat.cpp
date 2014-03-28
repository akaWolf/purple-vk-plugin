#include <debug.h>

#include "vk-buddy.h"
#include "vk-common.h"
#include "vk-utils.h"

#include "vk-chat.h"

int chat_id_to_conv_id(PurpleConnection* gc, uint64 chat_id)
{
    VkConnData* conn_data = get_conn_data(gc);
    for (const pair<int, uint64>& it: conn_data->chat_conv_ids)
        if (it.second == chat_id)
            return it.first;

    return 0;
}


uint64 conv_id_to_chat_id(PurpleConnection* gc, int conv_id)
{
    VkConnData* conn_data = get_conn_data(gc);
    for (const pair<int, uint64>& it: conn_data->chat_conv_ids)
        if (it.first == conv_id)
            return it.second;

    return 0;
}

int add_new_conv_id(PurpleConnection* gc, uint64 chat_id)
{
    int conv_id = 1;
    VkConnData* conn_data = get_conn_data(gc);
    for (const pair<int, uint64>& it: conn_data->chat_conv_ids)
        if (it.first >= conv_id)
            conv_id = it.first + 1;

    // We probably do not open more than one conversation per second, so the keys will be exhausted in 2 ** 31 seconds,
    // quite a lot of time.
    conn_data->chat_conv_ids.push_back({ conv_id, chat_id });
    return conv_id;
}


void remove_conv_id(PurpleConnection* gc, int conv_id)
{
    VkConnData* conn_data = get_conn_data(gc);
    erase_if(conn_data->chat_conv_ids, [=](const pair<int, uint64>& p) {
        return p.first == conv_id;
    });
}

namespace
{

// Used when user has duplicate name with other user in chat, appends some unique id.
string get_unique_display_name(PurpleConnection* gc, uint64 user_id)
{
    VkUserInfo* info = get_user_info(gc, user_id);
    if (!info)
        return user_name_from_id(user_id);

    // Return either "Name (nickname)" or "Name (id)"
    if (!info->domain.empty())
        return str_format("%s (%s)", info->real_name.data(), info->domain.data());
    else
        return str_format("%s (%" PRIu64 ")", info->real_name.data(), user_id);
}

// Checks if all users are the same as listed in info, returns false if differ.
bool are_equal_chat_users(PurpleConnection* gc, PurpleConvChat* conv, VkChatInfo* info)
{
    string_set names;
    for (uint64 user_id: info->participants) {
        string user_name = get_user_display_name(gc, user_id);
        if (contains(names, user_name)) {
            names.insert(get_unique_display_name(gc, user_id));
        } else {
            names.insert(user_name);
        }
    }
    const char* self_alias = purple_account_get_alias(purple_connection_get_account(gc));
    names.insert(self_alias);

    int users_size = 0;
    for (GList* it = purple_conv_chat_get_users(conv); it; it = it->next) {
        PurpleConvChatBuddy* cb = (PurpleConvChatBuddy*)it->data;
        if (!contains(names, purple_conv_chat_cb_get_name(cb)))
            return false;
        users_size++;
    }

    return (int)names.size() == users_size;
}

// Updates open conversation.
void update_open_chat_conv_impl(PurpleConnection* gc, PurpleConversation* conv, uint64 chat_id)
{
    VkChatInfo* info = get_chat_info(gc, chat_id);
    if (!info)
        return;

    if (purple_conversation_get_title(conv) != info->title.data())
        purple_conversation_set_title(conv, info->title.data());

    // Try to check if all users are present.
    if (!are_equal_chat_users(gc, PURPLE_CONV_CHAT(conv), info)) {
        vkcom_debug_info("Updating users in chat %" PRIu64 "\n", chat_id);

        purple_conv_chat_clear_users(PURPLE_CONV_CHAT(conv));

        for (uint64 user_id: info->participants) {
            string user_name = get_user_display_name(gc, user_id);

            // Check if we already have user with this name in chat.
            uint64 other_id = info->participant_names[user_name];
            if (other_id == 0) {
                info->participant_names[user_name] = user_id;
            } else if (other_id != user_id) {
                // We already have one user with this name in chat, get a unique one.
                user_name = get_unique_display_name(gc, user_id);
                info->participant_names[user_name] = user_id;
            }

            PurpleConvChatBuddyFlags flags;
            if (user_id == info->admin_id)
                flags = PURPLE_CBFLAGS_FOUNDER;
            else
                flags = PURPLE_CBFLAGS_NONE;
            purple_conv_chat_add_user(PURPLE_CONV_CHAT(conv), user_name.data(), "", flags, false);
        }

        // Add self
        const char* self_alias = purple_account_get_alias(purple_connection_get_account(gc));
        string self_name = str_format("%s (you)", self_alias);
        VkConnData* conn_data = get_conn_data(gc);
        info->participant_names[self_name] = conn_data->self_user_id();

        PurpleConvChatBuddyFlags flags;
        if (conn_data->self_user_id() == info->admin_id)
            flags = PURPLE_CBFLAGS_FOUNDER;
        else
            flags = PURPLE_CBFLAGS_NONE;
        purple_conv_chat_add_user(PURPLE_CONV_CHAT(conv), self_name.data(), "", flags, false);
    }
}

}

void open_chat_conv(PurpleConnection* gc, uint64 chat_id, const SuccessCb& success_cb)
{
    if (chat_id_to_conv_id(gc, chat_id)) {
        if (success_cb)
            success_cb();
        return;
    }

    add_chat_if_needed(gc, chat_id, [=] {
        VkChatInfo* info = get_chat_info(gc, chat_id);
        if (!info)
            return;

        string name = chat_name_from_id(chat_id);
        int conv_id = add_new_conv_id(gc, chat_id);
        PurpleConversation* conv = serv_got_joined_chat(gc, conv_id, name.data());
        vkcom_debug_info("Added chat conversation %d for %s\n", conv_id, name.data());

        update_open_chat_conv_impl(gc, conv, chat_id);

        if (success_cb)
            success_cb();
    });
}

void check_open_chat_convs(PurpleConnection* gc)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    VkConnData* conn_data = get_conn_data(gc);

    for (GList* it = purple_get_conversations(); it != NULL; it = it->next) {
        PurpleConversation* conv = (PurpleConversation *)it->data;

        if (purple_conversation_get_account(conv) != account)
            continue;

        if (purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_CHAT)
            continue;

        uint64 chat_id = chat_id_from_name(purple_conversation_get_name(conv));
        if (chat_id == 0)
            continue;

        int conv_id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv));
        conn_data->chat_conv_ids.push_back({ conv_id, chat_id });
    }
}


void update_open_chat_conv(PurpleConnection* gc, int conv_id)
{
    uint64 chat_id = conv_id_to_chat_id(gc, conv_id);
    if (chat_id == 0) {
        vkcom_debug_error("Trying to update unknown chat %d\n", conv_id);
        return;
    }

    PurpleConversation* conv = find_conv_for_id(gc, 0, chat_id);
    if (!conv) {
        vkcom_debug_error("Unable to find chat%" PRIu64 "\n", chat_id);
        return;
    }

    update_open_chat_conv_impl(gc, conv, chat_id);
}

void update_all_open_chat_convs(PurpleConnection* gc)
{
    VkConnData* conn_data = get_conn_data(gc);
    for (pair<int, uint64> p: conn_data->chat_conv_ids)
        update_open_chat_conv(gc, p.first);
}


uint64 find_user_id_from_conv(PurpleConnection* gc, int conv_id, const char* who)
{
    uint64 chat_id = conv_id_to_chat_id(gc, conv_id);
    if (chat_id == 0) {
        vkcom_debug_error("Asking for name %s in unknown chat %d\n", who, conv_id);
        return 0;
    }

    VkChatInfo* chat_info = get_chat_info(gc, chat_id);
    if (!chat_info) {
        vkcom_debug_error("Unknown chat%" PRIu64 "\n", chat_id);
        return 0;
    }

    uint64 user_id = map_at(chat_info->participant_names, who, 0);
    if (user_id == 0)
        vkcom_debug_error("Unknown user %s in chat%" PRIu64 "\n", who, chat_id);
    return user_id;
}
