#include <debug.h>

#include "miscutils.h"
#include "vk-api.h"
#include "vk-common.h"
#include "vk-message-send.h"
#include "vk-upload.h"
#include "vk-utils.h"

#include "vk-filexfer.h"

namespace
{

// Starts xfer. Finds out upload server URL, creates the full request for upload server and writes it.
// There seems to be no reason to call purple_xfer_start, so let's skip it.
void xfer_init(PurpleXfer* xfer);

} // End of anonymous namespace

PurpleXfer* new_xfer(PurpleConnection* gc, uint64 uid)
{
    if (uid == 0)
        return nullptr;

    PurpleXfer* xfer = purple_xfer_new(purple_connection_get_account(gc), PURPLE_XFER_SEND,
                                       buddy_name_from_uid(uid).data());

    xfer->data = new uint64(uid);

    // NOTE: We are lazy and do not implement "proper" sending file in buffer. We load the
    // contents of the file in xfer_start and hope that noone will be uploading DVD ISOs
    // to Vk.com. A proper way would be implementing xfer write_fnc.
    purple_xfer_set_init_fnc(xfer, xfer_init);

    return xfer;
}

namespace
{

// Returns string, containing md5sum of contents.
string compute_md5sum(const char* contents, gsize size)
{
    char* str = g_compute_checksum_for_data(G_CHECKSUM_MD5, (const unsigned char*)contents, size);
    string ret = str;
    g_free(str);
    return ret;
}

// Helper function, updating xfer progress and cancelling it if user has pressed cancel.
void xfer_upload_progress(PurpleXfer* xfer, PurpleHttpConnection* http_conn, int processed, int total)
{
    if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL) {
        purple_http_conn_cancel(http_conn);
        return;
    }

    size_t xfer_size = purple_xfer_get_size(xfer);
    // xfer_size is slightly less than total due to headers and stuff, so let's compensate.
    size_t sent = (processed > int(total - xfer_size)) ? processed + xfer_size - total : 0;
    purple_xfer_set_bytes_sent(xfer, sent);
    purple_xfer_update_progress(xfer);
}

// Sends given document to user and writes into the conversation about it.
// If resend is true, this url has already been sent.
void send_doc_url(PurpleConnection* gc, uint64 user_id, const string& url, bool resend)
{
    string attachemnt = parse_vkcom_attachments(url);
    send_im_attachment(gc, user_id, attachemnt);

    // Write information about uploaded file. so that user will be able to send the link to someone else.
    string who = buddy_name_from_uid(user_id);
    PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who.data(),
                                                                      purple_connection_get_account(gc));
    if (conv) {
        string message;
        if (resend)
            message = str_format("Sent file has already been uploaded and is permanently available at %s", url.data());
        else
            message = str_format("Sent file will be permanently available at %s", url.data());
        purple_conversation_write(conv, nullptr, message.data(), PURPLE_MESSAGE_SYSTEM, time(nullptr));
    }
}

// Sends document described by v to uid and save doc to uploaded_docs.
bool send_doc(PurpleConnection* gc, uint64 user_id, const VkUploadedDoc& doc, const picojson::value& v)
{
    if (!v.is<picojson::array>()) {
        purple_debug_error("prpl-vkcom", "Strange response from docs.save: %s\n", v.serialize().data());
        return false;
    }
    const picojson::value& d = v.get(0);
    if (!field_is_present<string>(d, "url")) {
        purple_debug_error("prpl-vkcom", "Strange response from docs.save: %s\n", v.serialize().data());
        return false;
    }

    const string& doc_url = d.get("url").get<string>();
    send_doc_url(gc, user_id, doc_url, false);

    // Store the uploaded document.
    uint64 doc_id = d.get("id").get<double>();
    VkConnData* conn_data = get_conn_data(gc);
    conn_data->uploaded_docs.push_back(doc);
    conn_data->uploaded_docs.back().id = doc_id;
    conn_data->uploaded_docs.back().url = doc_url;

    return true;
}

// Destructor for xfer.
void xfer_fini(PurpleXfer* xfer, char* contents)
{
    delete (uint64*)xfer->data;
    purple_xfer_unref(xfer);

    g_free(contents);
}

// Uploads document and sends it.
void start_uploading_doc(PurpleConnection* gc, PurpleXfer* xfer, const VkUploadedDoc& doc, char* contents)
{
    upload_doc_for_im(gc, doc.filename.data(), contents, doc.size, [=](const picojson::value& v) {
        uint64* uid = (uint64*)xfer->data;

        if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL) {
            purple_debug_info("prpl-vkcom", "Transfer has been cancelled by user\n");
        } else {
            if (send_doc(gc, *uid, doc, v)) {
                purple_xfer_set_completed(xfer, true);
                purple_xfer_end(xfer);
            } else {
                purple_xfer_cancel_remote(xfer);
            }
        }
        xfer_fini(xfer, contents);
    }, [=] {
        if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL)
            purple_debug_info("prpl-vkcom", "Transfer has been cancelled by user\n");
        else
            purple_xfer_cancel_remote(xfer);
        xfer_fini(xfer, contents);
    }, [=](PurpleHttpConnection* http_conn, int processed, int total) {
        xfer_upload_progress(xfer, http_conn, processed, total);
    });
}

// Calls docs.get for the current user and removes all the docs from uploaded_docs, which do not exist
// or do not match the stored parameters.
void clean_nonexisting_docs(PurpleConnection* gc, const SuccessCb& success_cb)
{
    shared_ptr<vector<VkUploadedDoc>> existing_docs{ new vector<VkUploadedDoc>() };

    VkConnData* conn_data = get_conn_data(gc);

    vk_call_api_items(gc, "docs.get", CallParams(), true, [=](const picojson::value& v) {
        if (!field_is_present<double>(v, "id") || !field_is_present<string>(v, "title")
                || !field_is_present<double>(v, "size") || !field_is_present<string>(v, "url")) {
            purple_debug_error("prpl-vkcom", "Strange response from docs.get: %s\n", v.serialize().data());
            return;
        }

        uint64 id = v.get("id").get<double>();
        const string& title = v.get("title").get<string>();
        uint64 size = v.get("size").get<double>();
        const string& url = v.get("url").get<string>();

        for (const VkUploadedDoc& doc: conn_data->uploaded_docs) {
            if (doc.id == id) {
                if (doc.filename == title && doc.size == size && doc.url == url)
                    existing_docs->push_back(doc);
                else
                    purple_debug_info("prpl-vkcom", "Document %" PRIu64 " changed either title, size or url, "
                                      "removing from uploaded\n", id);
            }
        }
    }, [=]() {
        int size_diff = conn_data->uploaded_docs.size() - existing_docs->size();
        if (size_diff > 0)
            purple_debug_info("prpl-vkcom", "%d docs removed from uploaded\n", size_diff);
        conn_data->uploaded_docs = *existing_docs;

        if (success_cb)
            success_cb();
    }, [=](const picojson::value& v) {
        purple_debug_warning("prpl-vkcom", "Error in docs.get: %s, removing all docs\n", v.serialize().data());
        conn_data->uploaded_docs = *existing_docs;

        if (success_cb)
            success_cb();
    });
}

// Either finds matching doc, checks that it exists and sends it or uploads new doc.
void find_or_upload_doc(PurpleConnection* gc, PurpleXfer* xfer, const VkUploadedDoc& doc, char* contents)
{
    // We have a concurrency problem here: if the document is uploaded and added during the
    // call to clean_nonexisting_docs (between calling docs.get and parsing the results) it will
    // not be added to uploaded_docs. It is a minor problem (the document will be reuploaded
    // the next time it is added) and all this "check if doc still exists" approach is
    // non-concurrency-proof already.
    clean_nonexisting_docs(gc, [=] {
        VkConnData* conn_data = get_conn_data(gc);
        for (const VkUploadedDoc& up: conn_data->uploaded_docs) {
            if (up.filename == doc.filename && up.size == doc.size && up.md5sum == doc.md5sum) {
                purple_debug_info("prpl-vkcom", "Filename, size and md5sum matches the doc %" PRIu64 ", resending it.\n", up.id);

                uint64 user_id = *(uint64*)xfer->data;
                send_doc_url(gc, user_id, up.url, true);

                purple_xfer_set_completed(xfer, true);
                purple_xfer_end(xfer);
                xfer_fini(xfer, contents);
                return;
            }
        }

        start_uploading_doc(gc, xfer, doc, contents);
    });
}

void xfer_init(PurpleXfer* xfer)
{
    assert(purple_xfer_get_type(xfer) == PURPLE_XFER_SEND);
    PurpleConnection* gc = purple_account_get_connection(purple_xfer_get_account(xfer));

    const char* filepath = purple_xfer_get_local_filename(xfer);
    const char* filename = purple_xfer_get_filename(xfer);

    // Load all contents in memory.
    char* contents;
    gsize size;
    if (!g_file_get_contents(filepath, &contents, &size, nullptr)) {
        purple_debug_error("prpl-vkcom", "Unable to read file %s\n", filepath);
        return;
    }

    // We manually increase reference count for xfer, so that it does not die without us noticing it.
    // Xfer can be cancelled locally anytime, which may lead to error callback getting called or not called.
    // The former happens if the user cancelled xfer before xfer_upload_progress has been called once again.
    purple_xfer_ref(xfer);

    VkUploadedDoc doc;
    doc.filename = filename;
    doc.size = size;
    doc.md5sum = compute_md5sum(contents, size);

    find_or_upload_doc(gc, xfer, doc, contents);
}

} // End of anonymous namespace
