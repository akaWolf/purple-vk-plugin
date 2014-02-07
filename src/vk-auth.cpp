#include <cstring>
#include <debug.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>

#include "httputils.h"
#include "miscutils.h"

#include "vk-auth.h"

namespace
{

// Collects all information required about a form in html page
struct HtmlForm
{
    string action_url;
    string method;
    string_map params; // Mapping from param name to param value if one exists
};

// Finds <form> element in the document
xmlNode* find_form_element(xmlDoc* doc);
// Finds HTML form (it must be the only one) in the document and return its parameters.
HtmlForm find_html_form(xmlDoc* doc);
// Returns POST headers for given url and data to be sent in x-www-form-urlencoded
PurpleHttpRequest* prepare_form_request(const HtmlForm& form);

xmlNode* find_form_element(xmlDoc* doc)
{
    xmlXPathContext* context = xmlXPathNewContext(doc);
    xmlXPathObject* result = xmlXPathEvalExpression((xmlChar*)"//form", context);
    xmlXPathFreeContext(context);
    xmlNodeSet* node_set = result->nodesetval;

    if (xmlXPathNodeSetGetLength(node_set) != 1) {
        purple_debug_error("prpl-vkcom", "Wrong number of <form>s in given html: %d\n",
                           xmlXPathNodeSetGetLength(node_set));
        return nullptr;
    }

    return node_set->nodeTab[0];
}

HtmlForm find_html_form(xmlDoc* doc)
{
    HtmlForm ret;

    xmlNode* form = find_form_element(doc);
    if (!form)
        return ret;

    ret.action_url = get_xml_node_prop(form, "action");
    ret.method = get_xml_node_prop(form, "method", "get");
    to_upper(ret.method);

    xmlXPathContext* context = xmlXPathNewContext(doc);
    context->node = form;
    xmlXPathObject* result = xmlXPathEvalExpression((xmlChar*)"//input", context);
    xmlXPathFreeContext(context);
    xmlNodeSet* node_set = result->nodesetval;

    for (int i = 0; i < node_set->nodeNr; i++) {
        xmlNode* input = node_set->nodeTab[i];
        string type = get_xml_node_prop(input, "type");
        if (type != "hidden" && type != "text" && type != "password")
            continue;
        string name = get_xml_node_prop(input, "name");
        string value = get_xml_node_prop(input, "value");
        ret.params[name] = value;
    }

    return ret;
}

PurpleHttpRequest* prepare_form_request(const HtmlForm& form)
{
    PurpleHttpRequest* req = purple_http_request_new(form.action_url.data());

    purple_http_request_set_method(req, form.method.data());
    purple_http_request_header_add(req, "Content-type", "application/x-www-form-urlencoded");

    string data = urlencode_form(form.params);
    purple_http_request_set_contents(req, data.data(), -1);

    return req;
}

// Struct, containing all data, regarding authentication.
struct AuthData
{
    PurpleConnection* gc;
    string email;
    string password;
    string client_id;
    string scope;
    bool imitate_mobile_client;

    AuthSuccessCb success_cb;
    ErrorCb error_cb;
};

typedef std::shared_ptr<AuthData> AuthDataPtr;

//    void on_success(const string& access_token, const string& uid);
//    // Called upon failure, destroys this.
//    void on_error(PurpleConnectionError error, const string& error_string);

// Starts authentication process.
void start_auth(const AuthDataPtr& data);

// First part of auth process: retrieves login page, finds relevant form with username (email) and password
// and submits it.
void on_fetch_vk_oauth_form(const AuthDataPtr& data, PurpleHttpConnection* http_conn,
                            PurpleHttpResponse* response);
// Second part of auth process: retrieves "confirm access" page and submits form. This part may be skipped.
void on_fetch_vk_confirmation_form(const AuthDataPtr& data, PurpleHttpConnection* http_conn,
                                   PurpleHttpResponse* response);
// Last part of auth process: retrieves access token. We either arrive here upon success from confirmation
// page or upon error (when url starts with "https://oauth.vk.com/blank.html").
void on_fetch_vk_access_token(const AuthDataPtr& data, PurpleHttpConnection* http_conn,
                              PurpleHttpResponse*);


// Called upon auth error.
void on_error(const AuthDataPtr& data, PurpleConnectionError error, const string& error_string)
{
    purple_connection_error_reason(data->gc, error, error_string.data());
    if (data->error_cb)
        data->error_cb();
}

const char api_version[] = "5.8";
const char mobile_user_agent[] = "Mozilla/5.0 (Mobile; rv:17.0) Gecko/17.0 Firefox/17.0";
const char desktop_user_agent[] = "Mozilla/5.0 (Windows NT 6.1; Win64; x64; rv:25.0) Firefox/25.0";

void start_auth(const AuthDataPtr& data)
{
    assert(data->success_cb);
    purple_connection_update_progress(data->gc, "Connecting", 0, 4);
    purple_debug_info("prpl-vkcom", "Starting authentication\n");

    string url = str_format("https://oauth.vk.com/oauth/authorize?redirect_uri=https://oauth.vk.com/blank.html"
                            "&response_type=token&client_id=%s&scope=%s&display=page&v=%s",
                            data->client_id.data(), data->scope.data(), api_version);
    http_get(data->gc, url, [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
        on_fetch_vk_oauth_form(data, http_conn, response);
    });
}

void on_fetch_vk_oauth_form(const AuthDataPtr& data, PurpleHttpConnection* http_conn,
                            PurpleHttpResponse* response)
{
    purple_connection_update_progress(data->gc, "Connecting", 1, 4);
    purple_debug_info("prpl-vkcom", "Fetched login page\n");

    if (!purple_http_response_is_successful(response)) {
        purple_debug_error("prpl-vkcom", "Error retrieving login page: %s\n", purple_http_response_get_error(response));
        on_error(data, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Error retrieving login page");
        return;
    }

    const char* page_data = purple_http_response_get_data(response, nullptr);
    xmlDoc* doc = htmlReadDoc((xmlChar*)page_data, nullptr, "utf-8", HTML_PARSE_RECOVER | HTML_PARSE_NOBLANKS
                              | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) {
        purple_debug_error("prpl-vkcom", "Unable to parse login form HTML: %s\n", page_data);
        on_error(data, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }
    HtmlForm form = find_html_form(doc);
    xmlFreeDoc(doc);

    if (form.action_url.empty()) {
        purple_debug_error("prpl-vkcom", "Error finding form in login page: %s\n", page_data);
        on_error(data, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }

    if (!map_update(form.params, "email", data->email)) {
        purple_debug_error("prpl-vkcom", "Login form does not contain email: %s\n", page_data);
        on_error(data, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }
    if (!map_update(form.params, "pass", data->password)) {
        purple_debug_error("prpl-vkcom", "Login form does not contain pass: %s\n", page_data);
        on_error(data, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }

    PurpleHttpRequest* request = prepare_form_request(form);
    if (data->imitate_mobile_client)
        purple_http_request_header_add(request, "User-Agent", mobile_user_agent);
    else
        purple_http_request_header_add(request, "User-Agent", desktop_user_agent);
    http_request_copy_cookie_jar(request, http_conn);
    http_request_update_on_redirect(data->gc, request, [=](PurpleHttpConnection* new_conn, PurpleHttpResponse* new_response) {
        on_fetch_vk_confirmation_form(data, new_conn, new_response);
    });
    purple_http_request_unref(request);
}

void on_fetch_vk_confirmation_form(const AuthDataPtr& data, PurpleHttpConnection* http_conn,
                                   PurpleHttpResponse* response)
{
    purple_connection_update_progress(data->gc, "Connecting", 2, 4);

    // Check if url contains "https://oauth.vk.com/blank.html"
    const char* url = purple_http_request_get_url(purple_http_conn_get_request(http_conn));
    // Check if we must skip the confirmation form and get access token straight.
    if (g_str_has_prefix(url, "https://oauth.vk.com/blank.html")) {
        on_fetch_vk_access_token(data, http_conn, response);
        return;
    }

    purple_debug_info("prpl-vkcom", "Fetched login confirmation page");
    if (!purple_http_response_is_successful(response)) {
        purple_debug_error("prpl-vkcom", "Error retrieving login confirmation page: %s\n",
                           purple_http_response_get_error(response));
        on_error(data, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Error retrieving login confirmation page");
        return;
    }

    const char* page_data = purple_http_response_get_data(response, nullptr);
    xmlDoc* doc = htmlReadDoc((xmlChar*)page_data, nullptr, "utf-8", HTML_PARSE_RECOVER | HTML_PARSE_NOBLANKS
                              | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) {
        purple_debug_error("prpl-vkcom", "Unable to parse confirmation form HTML: %s\n", page_data);
        on_error(data, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }
    HtmlForm form = find_html_form(doc);
    xmlFreeDoc(doc);

    if (form.action_url.empty()) {
        purple_debug_error("prpl-vkcom", "Error finding form in login confirmation page: %s\n", page_data);
        on_error(data, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }

    PurpleHttpRequest* request = prepare_form_request(form);
    if (data->imitate_mobile_client)
        purple_http_request_header_add(request, "User-Agent", mobile_user_agent);
    else
        purple_http_request_header_add(request, "User-Agent", desktop_user_agent);
    http_request_copy_cookie_jar(request, http_conn);
    http_request_update_on_redirect(data->gc, request, [=](PurpleHttpConnection* new_conn, PurpleHttpResponse* new_response) {
        on_fetch_vk_access_token(data, new_conn, new_response);
    });
    purple_http_request_unref(request);
}

// Last part of auth process: retrieves access token. We either arrive here upon success from confirmation
// page or upon error (when url starts with "https://oauth.vk.com/blank.html").
void on_fetch_vk_access_token(const AuthDataPtr& data, PurpleHttpConnection* http_conn, PurpleHttpResponse*)
{
    purple_connection_update_progress(data->gc, "Connecting", 3, 4);
    purple_debug_info("prpl-vkcom", "Fetched access token URL\n");

    // Check if url contains "https://oauth.vk.com/blank.html"
    const char* url = purple_http_request_get_url(purple_http_conn_get_request(http_conn));
    if (!g_str_has_prefix(url, "https://oauth.vk.com/blank.html")) {
        purple_debug_info("prpl-vkcom", "Error while getting access token: ended up with url %s\n", url);
        on_error(data, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, "Wrong username or password");
        return;
    }

    const char* url_params = strchr(url, '#') + 1;
    string_map params = parse_urlencoded_form(url_params);
    string access_token = params["access_token"];
    if (access_token.empty()) {
        purple_debug_error("prpl-vkcom", "access_token not present in %s\n", url_params);
        on_error(data, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    } else {
        purple_connection_set_state(data->gc, PURPLE_CONNECTED);
        data->success_cb(params["access_token"], params["user_id"]);
    }
}

} // End of anonymous namespace

void vk_auth_user(PurpleConnection* gc, const string& email, const string& password, const string& client_id, const string& scope,
                  const AuthSuccessCb& success_cb, const ErrorCb& error_cb)
{
    AuthDataPtr data{ new AuthData };
    data->gc = gc;
    data->email = email;
    data->password = password;
    data->client_id = client_id;
    data->scope = scope;
    data->success_cb = success_cb;
    data->error_cb = error_cb;

    PurpleAccount* account = purple_connection_get_account(gc);
    data->imitate_mobile_client = purple_account_get_bool(account, "imitate_mobile_client", false);

    start_auth(data);
}
