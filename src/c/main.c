/*
 * Copyright (c) 2018
 * IoTech Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include "edgex/devsdk.h"
#include "edgex/device-mgmt.h"
#include "edgex/eventgen.h"
#include "open62541/open62541.h"

#define __STDC_FORMAT_MACROS

#include <inttypes.h>

#include <unistd.h>
#include <signal.h>
#include <string.h>

#define ERR_CHECK(x) if ((x).code) { fprintf (stderr, "Error: %d: %s\n", (x).code, (x).reason); edgex_device_service_free (service); free_subs(impl->subs); free (impl); return (x).code; }

#define PROTOCOL "opc.tcp://"

#define UA_SCANF_GUID_DATA(GUID) &(GUID).data1, &(GUID).data2, &(GUID).data3, \
        &(GUID).data4[0], &(GUID).data4[1], &(GUID).data4[2], &(GUID).data4[3], \
        &(GUID).data4[4], &(GUID).data4[5], &(GUID).data4[6], &(GUID).data4[7]

static edgex_device_service *service;

typedef struct subscription_info
{
  uint32_t subId;
  uint32_t monId;
  char *devname;
  char *name;
  struct subscription_info *next;
} subscription_info;

typedef struct client_context
{
  void *driver;
  const char *devname;
} client_context;

typedef struct opcua_connection
{
  struct opcua_connection *next;
  UA_Client *client;
  char *addr_id;
  char *endpoint;
  pthread_mutex_t mutex;
  int reconnect_count;
} opcua_connection;

typedef struct ua_addr
{
  char *addr_id;
  struct ua_addr *next;
} ua_addr;

typedef struct ua_conn_addr_status
{
  ua_addr *front;
  ua_addr *back;
  int length;
  pthread_mutex_t mutex;
} ua_conn_addr_status;

typedef struct opcua_driver
{
  iot_logger_t *lc;
  pthread_mutex_t mutex;
  struct opcua_connection *conn_front;
  struct opcua_connection *conn_back;
  int conn_length;
  struct ua_conn_addr_status add_conn_status;
  subscription_info *subs;
} opcua_driver;

static sig_atomic_t running = true;

static void inthandler(int i)
{
  running = (i != SIGINT);
}

static edgex_device_commandresult opcua_to_edgex(UA_Variant *value,
  opcua_driver *uadr);

/* OPCUA General */

static void free_subs(subscription_info *sub)
{
  subscription_info *tmp = sub, *tmp2;

  while (tmp)
  {
    tmp2 = tmp->next;
    free(tmp->name);
    free(tmp->devname);
    free(tmp);
    tmp = tmp2;
  }
  return;
}

static void deleteSubscriptionCallback(UA_Client *client,
  UA_UInt32 subscriptionId, void *subscriptionContext)
{
  client_context *clientContext;
  opcua_driver *uadr;
  subscription_info *item = NULL, *prev = NULL;

  clientContext = (client_context *)UA_Client_getContext(client);
  if (!clientContext)
    return;

  uadr = (opcua_driver *)clientContext->driver;
  if (!uadr->subs)
  {
    iot_log_info(uadr->lc, "No subscriptions set up - ignoring");
    return;
  }

  /* Deal with any occurrences at head of list */
  item = uadr->subs;
  while (item && (item->subId == subscriptionId))
  {
      uadr->subs = item->next;
      free(item->name);
      free(item->devname);
      free(item);
      item = uadr->subs;
  }

  /* Deal with rest of list */
  while (item)
  {
    while (item && (item->subId != subscriptionId))
    {
      prev = item;
      item = item->next;
    }

    if (!item)
      return;

    /* Unlink and free */
    prev->next = item->next;
    free(item->name);
    free(item->devname);
    free(item);
    item = prev->next;
  }
  return;
}

/* Generic handler to post readings from monitored items */
static void subscription_handler(UA_Client *client, UA_UInt32 subId,
  void *subContext, UA_UInt32 monId, void *monContext, UA_DataValue *value)
{
  client_context *clientContext;
  opcua_driver *uadr;
  subscription_info *item = NULL;
  edgex_device_commandresult results[1];

  clientContext = (client_context *)UA_Client_getContext(client);
  if (!clientContext)
    return;

  uadr = clientContext->driver;
  pthread_mutex_lock(&uadr->mutex);
  if (!uadr->subs)
  {
    pthread_mutex_unlock(&uadr->mutex);
    iot_log_error(uadr->lc, "No subscriptions set up - ignoring");
    return;
  }

  /* Find the relevant subscription */
  for (item = uadr->subs; item; item = item->next)
  {
    if ((item->monId == monId) && (item->subId == subId))
      break;
  }
  pthread_mutex_unlock(&uadr->mutex);

  if (!item)
  {
    iot_log_error(uadr->lc, "No subscriptions id match");
    return;
  }
  if (!item->name || !item->devname)
  {
    iot_log_error(uadr->lc, "No subscription name found");
    return;
  }

  results[0] = opcua_to_edgex(&value->value, uadr);
  results[0].origin = 0; /* Timestamp provided is int64, not uint64 */

  edgex_device_post_readings(service, item->devname, item->name, results);
}

static const UA_NodeId get_subscription_nodeid(edgex_deviceresource *resource)
{
  int subscription = 0;
  char *strID = "";
  char *nsIndex = "";
  char *IDType = "";
  char * endpt;
  UA_NodeId nodeId = UA_NODEID_NULL;
  edgex_nvpairs *nvp = resource->attributes;

  while (nvp != NULL)
  {
    if (!strcmp(nvp->name, "nodeID"))
      strID = nvp->value;
    else if (!strcmp(nvp->name, "nsIndex"))
      nsIndex = nvp->value;
    else if (!strcmp(nvp->name, "IDType"))
      IDType = nvp->value;
    else if (!strcmp(nvp->name, "monitored") && !strcmp(nvp->value, "True"))
      subscription = 1;
    nvp = nvp->next;
  }

  if (!subscription)
    return nodeId;

  if (strcmp(IDType,"STRING") == 0)
  {
    nodeId = UA_NODEID_STRING((UA_UInt16)strtol(nsIndex,&endpt,10), strID);
  }
  else if (strcmp(IDType,"NUMERIC") == 0)
  {
    nodeId = UA_NODEID_NUMERIC((UA_UInt16)strtol(nsIndex,&endpt,10),
      (UA_UInt32)strtol(strID,&endpt,10));
  }
  else if (strcmp(IDType,"BYTESTRING") == 0)
  {
    nodeId = UA_NODEID_BYTESTRING((UA_UInt16)strtol(nsIndex,&endpt,10),
      strID);
  }
  else if (strcmp(IDType,"GUID") == 0)
  {
    UA_Guid guid;
    UA_Guid_init(&guid);
    sscanf(strID,UA_PRINTF_GUID_FORMAT,UA_SCANF_GUID_DATA(guid));
    nodeId = UA_NODEID_GUID((UA_UInt16)strtol(nsIndex,&endpt,10), guid);
  }

  return nodeId;
}

static void setup_subscriptions(UA_Client *client)
{
  UA_NodeId node = UA_NODEID_NULL;
  client_context *clientContext;
  opcua_driver *uadr;
  edgex_device *device = NULL;
  edgex_deviceprofile *profile = NULL;
  edgex_deviceresource *resource = NULL;
  subscription_info *item = NULL;
  UA_CreateSubscriptionRequest request;
  UA_CreateSubscriptionResponse response;
  UA_MonitoredItemCreateRequest monRequest;
  UA_MonitoredItemCreateResult monResponse;

  clientContext = (client_context *)UA_Client_getContext(client);
  if (!clientContext)
    return;

  uadr = clientContext->driver;

  device = edgex_device_get_device_byname (service, clientContext->devname);
  if (!device)
  {
    iot_log_error(uadr->lc, "Couldn't find device");
    return;
  }
  if (!device->profile)
  {
    iot_log_error(uadr->lc, "Couldn't find device profile");
    return;
  }
  /* assume only one profile for now. Could create a new subscription for each profile */
  profile = device->profile;

  /* Create a subscription */
  request = UA_CreateSubscriptionRequest_default();
  response = UA_Client_Subscriptions_create(client, request, NULL, NULL,
    deleteSubscriptionCallback);

  if (response.responseHeader.serviceResult != UA_STATUSCODE_GOOD)
    return;

  for (resource=profile->device_resources; resource;
    resource=resource->next, node=UA_NODEID_NULL)
  {
    node = get_subscription_nodeid(resource);
    if (!UA_NodeId_equal(&node, &UA_NODEID_NULL))
    {
      /* Add a MonitoredItem */
      monRequest = UA_MonitoredItemCreateRequest_default(node);
      monResponse = UA_Client_MonitoredItems_createDataChange(client,
        response.subscriptionId, UA_TIMESTAMPSTORETURN_BOTH,
        monRequest, NULL, subscription_handler, NULL);
      if (monResponse.statusCode == UA_STATUSCODE_GOOD)
      {
        item = (subscription_info *)malloc(sizeof(subscription_info));
        memset(item, 0, sizeof(subscription_info));
        item->name = calloc(strlen(resource->name)+1, sizeof(char));
        strcpy(item->name, resource->name);
        item->devname = calloc(strlen(device->name)+1, sizeof(char));
        strcpy(item->devname, device->name);
        item->subId = response.subscriptionId;
        item->monId = monResponse.monitoredItemId;
        pthread_mutex_lock(&uadr->mutex);
        item->next = uadr->subs;
        uadr->subs = item;
        pthread_mutex_unlock(&uadr->mutex);
        iot_log_info(uadr->lc, "Setting up subscription for %s", item->name);
      }
      else
      {
        iot_log_error(uadr->lc, "Failed to set up monitored item %s",
          resource->name);
      }
    }
  }
  edgex_device_free_device(device);
}

/*
 * Callback function to allow creation of subscriptions once connection to
 * server has been established.
 */
static void stateCallback(UA_Client *client, UA_ClientState clientState)
{
  switch(clientState)
  {
    case UA_CLIENTSTATE_SESSION:
      /* A new session was created. We need to create any subscriptions. */
      setup_subscriptions(client);
      break;
    case UA_CLIENTSTATE_SESSION_RENEWED:
      /* The session was renewed. We don't need to recreate subscriptions. */
    default:
      /* Ignore other session state changes for now. */
      break;
  }
  return;
}

/* Creates the opcua channel and session */
static UA_StatusCode opcua_connect(UA_Client *client, char *endpoint)
{
  UA_StatusCode retval;
  /* TODO: When supported connect with user & password */
  retval = UA_Client_connect(client, endpoint);
  return retval;
}

/* Creates and returns a new opcua_connection */
static opcua_connection *create_opcua_connection(opcua_driver *uadr,
    const char *devname, edgex_protocols *protocol)
{
  UA_Client *client = NULL;
  const char *address = NULL;
  uint64_t port = 0;
  const char *path = NULL;
  edgex_nvpairs *pos = protocol->properties;

  for (const edgex_protocols *current = protocol; current;
    current = current->next)
  {
    if (!strcmp(protocol->name, "OPC-UA"))
    {
      /*
       * Need to ensure we have enough information specified in order to
       * establish the connection.
       */
      while (pos)
      {
        if (!strcmp(pos->name, "Address"))
        {
          if (!address)
            address = pos->value;
        }
        else if (!strcmp(pos->name, "Port"))
        {
          if (!port)
            port = strtol(pos->value, NULL, 10);
        }
        else if (!strcmp(pos->name, "Path"))
        {
          if (!path)
            path = pos->value;
        }
        pos = pos->next;
      }
      break;
    }
  }

  if (!address || !path || !port)
  {
    iot_log_error(uadr->lc, "Failed to create client - missing config info");
    return NULL;
  }
  iot_log_debug(uadr->lc,
    "Got connection info of addr %s port %lu path %s\n", address, port, path);

  /* Create and return the opcua_connection */
  opcua_connection *conn = malloc(sizeof(opcua_connection));
  memset(conn, 0, sizeof(opcua_connection));

  /* Construct the endpoint */
  /* Fix magic const */
  char *endpoint = malloc(strlen(PROTOCOL) + strlen(address) + 20 * sizeof(char) + strlen(path));
  sprintf(endpoint, "%s%s:%"PRIu64"%s", PROTOCOL, address, port, path);

  /* create the client */
  UA_ClientConfig config = UA_ClientConfig_default;
  /*
   * Need to attach driver to clientContext to allow us to retrieve the
   * structure during stateCallback.
   * Also add in device name to allow us to post readings to EdgeX.
   */
  client_context *context = (void *)malloc(sizeof(client_context));
  context->driver = (void *)uadr;
  context->devname = devname;
  config.clientContext = (void *)context;
  /* Set stateCallback, where subscriptions will be set up */
  config.stateCallback = stateCallback;
  client = UA_Client_new(config);
  if (client == NULL)
  {
    iot_log_error(uadr->lc, "Failed to create client");
    conn->client = NULL;
    free(context);
    free(endpoint);
    return conn;
  }

  /* make the connection */
  conn->endpoint = endpoint;
  UA_StatusCode retval = opcua_connect(client, endpoint);
  if (retval != UA_STATUSCODE_GOOD)
  {
    iot_log_error(uadr->lc, "Client failed to connect. Status Code: %s",
      UA_StatusCode_name(retval));
    free(context);
    UA_Client_delete(client);
    return conn;
  }

  conn->client = client;
  conn->addr_id = strdup(devname);
  pthread_mutex_init(&conn->mutex, NULL);
  iot_log_info(uadr->lc,
    "Created new OPC-UA connection at endpoint {%s} with id {%s}",
    endpoint, conn->addr_id);
  return conn;
}

/* Looks for an opcua_connection associated with the edgex_protocols entry. If
 * an existing connection is not found a new connection is created and
 * established. Returns the opcua_connection
 */
static opcua_connection *find_opcua_connection(opcua_driver *uadr,
    const char *devname, edgex_protocols *protocol)
{
  /* Check if the opcua_connection can be found */
  if (uadr->conn_length > 0)
  {
    opcua_connection *curr = uadr->conn_front;
    for (uint32_t i = 0; i < uadr->conn_length; i++)
    {
      if (strcmp(curr->addr_id, devname) == 0)
      {
        iot_log_debug(uadr->lc, "Found Existing opcua_connection: %s",
          curr->addr_id);
        return curr;
      }
      curr = curr->next;
    }
  }

  /* If the opcua_connection can't be found, or there aren't any, create one */
  iot_log_info(uadr->lc, "Creating new OPC-UA connection.");
  opcua_connection *ua_conn = create_opcua_connection(uadr, devname, protocol);
  if (!ua_conn)
    return NULL;
  if (ua_conn->client == NULL)
    return ua_conn;

  pthread_mutex_lock(&uadr->mutex);
  if (uadr->conn_length > 0)
  {
    pthread_mutex_lock(&uadr->conn_back->mutex);
    uadr->conn_back->next = ua_conn;
    pthread_mutex_unlock(&uadr->conn_back->mutex);
    uadr->conn_back = ua_conn;
  }
  else if (uadr->conn_length == 0)
  {
    uadr->conn_front = ua_conn;
    uadr->conn_back = ua_conn;
  }
  uadr->conn_length++;
  pthread_mutex_unlock(&uadr->mutex);

  return ua_conn;
}

/* Query the attributes to get the correct node ID */
static const UA_NodeId get_ua_nodeid(edgex_device_commandrequest request)
{
  char *strID = "";
  char *namespaceIndex = "";
  char *IDType = "";

  const edgex_nvpairs *nvp = request.attributes;
  while (nvp != NULL)
  {
    if (strcmp(nvp->name, "nodeID") == 0)
      strID = nvp->value;
    else if (strcmp(nvp->name, "nsIndex") == 0)
      namespaceIndex = nvp->value;
    else if (strcmp(nvp->name, "IDType") == 0)
      IDType = nvp->value;
    nvp = nvp->next;
  }

  char * endpt;
  UA_NodeId nodeId = UA_NODEID_NULL;

  if (strcmp(IDType,"STRING") == 0)
  {
    nodeId = UA_NODEID_STRING((UA_UInt16)strtol(namespaceIndex,&endpt,10), strID);
  }
  else if (strcmp(IDType,"NUMERIC") == 0)
  {
    nodeId = UA_NODEID_NUMERIC((UA_UInt16)strtol(namespaceIndex,&endpt,10),
      (UA_UInt32)strtol(strID,&endpt,10));
  }
  else if (strcmp(IDType,"BYTESTRING") == 0)
  {
    nodeId = UA_NODEID_BYTESTRING((UA_UInt16)strtol(namespaceIndex,&endpt,10),
      strID);
  }
  else if (strcmp(IDType,"GUID") == 0)
  {
    UA_Guid guid;
    UA_Guid_init(&guid);
    sscanf(strID,UA_PRINTF_GUID_FORMAT,UA_SCANF_GUID_DATA(guid));
    nodeId = UA_NODEID_GUID((UA_UInt16)strtol(namespaceIndex,&endpt,10), guid);
  }

  return nodeId;
}

/* Switch over the OPCUA data types and map those applicable to edgex types */
static edgex_device_commandresult opcua_to_edgex(UA_Variant *value,
  opcua_driver *uadr)
{
  edgex_device_commandresult result;
  memset(&result, 0, sizeof(edgex_device_commandresult));

  /*
   * If we've connected to the server too quickly during it's start up, it is
   * possible to get a malformed UA_Variant passed to us - attempt to deal with
   * this gracefully.
   */
  if (!value || !value->type || !value->type->typeIndex)
  {
    iot_log_debug(uadr->lc, "Malformed UA_Variant.");
    return result;
  }

  switch (value->type->typeIndex)
  {
    case UA_TYPES_BOOLEAN:
      result.type = Bool;
      result.value.bool_result = *(UA_Boolean *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %d.",
                     value->type->typeName, result.value.bool_result);
      break;
    case UA_TYPES_STRING:
      result.type = String;
      UA_String data = *(UA_String *)value->data;
      char *convert = (char *)UA_malloc(sizeof (char) * data.length + 1);
      memcpy(convert, data.data, data.length);
      convert[data.length] = '\0';
      result.value.string_result = convert;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %s.",
                     value->type->typeName, result.value.string_result);
      break;
    case UA_TYPES_BYTE:
      result.type = Uint8;
      result.value.ui8_result = *(UA_Byte *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %u.",
                     value->type->typeName, result.value.ui8_result);
      break;
    case UA_TYPES_UINT16:
      result.type = Uint16;
      result.value.ui16_result = *(UA_UInt16 *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %u.",
                     value->type->typeName, result.value.ui16_result);
      break;
    case UA_TYPES_UINT32:
      result.type = Uint32;
      result.value.ui32_result = *(UA_UInt32 *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %u.",
                     value->type->typeName, result.value.ui32_result);
      break;
    case UA_TYPES_UINT64:
      result.type = Uint64;
      result.value.ui64_result = *(UA_UInt64 *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %lu.",
                     value->type->typeName, result.value.ui64_result);
      break;
    case UA_TYPES_SBYTE:
      result.type = Int8;
      result.value.i8_result = *(UA_SByte *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %d.",
                     value->type->typeName, result.value.i8_result);
      break;
    case UA_TYPES_INT16:
      result.type = Int16;
      result.value.i16_result = *(UA_Int16 *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %d.",
                     value->type->typeName, result.value.i16_result);
      break;
    case UA_TYPES_INT32:
      result.type = Int32;
      result.value.i32_result = *(UA_Int32 *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %d.",
                     value->type->typeName, result.value.i32_result);
      break;
    case UA_TYPES_DATETIME:
    case UA_TYPES_INT64:
      result.type = Int64;
      result.value.i64_result = *(UA_Int64 *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %ld.",
                     value->type->typeName, result.value.i64_result);
      break;
    case UA_TYPES_FLOAT:
      result.type = Float32;
      result.value.f32_result = *(UA_Float *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %f.",
                     value->type->typeName, result.value.f32_result);
      break;
    case UA_TYPES_DOUBLE:
      result.type = Float64;
      result.value.f64_result = *(UA_Double *)value->data;
      iot_log_debug(uadr->lc, "Reading data of type %s with value %lf.",
                     value->type->typeName, result.value.f64_result);
      break;
    default:
      iot_log_error(uadr->lc, "Type %s not supported!",value->type->typeName);
      break;
  }
  return result;
}

/* Switch over edgex types, map to OPC-UA */
static UA_Variant *edgex_to_opcua(edgex_device_commandresult result,
  opcua_driver *uadr)
{
  UA_Variant *value = UA_Variant_new();
  switch (result.type)
  {
    case Bool:
      UA_Variant_setScalar(value, &result.value.bool_result,
        &UA_TYPES[UA_TYPES_BOOLEAN]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %d.",
                     value->type->typeName, result.value.bool_result);
      break;
    case String:
    {
      UA_String string = UA_STRING(result.value.string_result);
      UA_Variant_setScalarCopy(value, &string, &UA_TYPES[UA_TYPES_STRING]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %s.",
                     value->type->typeName, result.value.string_result);
      break;
    }
    case Uint8:
      UA_Variant_setScalarCopy(value, &result.value.ui8_result,
        &UA_TYPES[UA_TYPES_BYTE]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %u.",
                     value->type->typeName, result.value.ui8_result);
      break;
    case Uint16:
      UA_Variant_setScalarCopy(value, &result.value.ui16_result,
        &UA_TYPES[UA_TYPES_UINT16]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %u.",
                     value->type->typeName, result.value.ui16_result);
      break;
    case Uint32:
      UA_Variant_setScalarCopy(value, &result.value.ui32_result,
        &UA_TYPES[UA_TYPES_UINT32]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %u.",
                     value->type->typeName, result.value.ui32_result);
      break;
    case Uint64:
      UA_Variant_setScalarCopy(value, &result.value.ui64_result,
        &UA_TYPES[UA_TYPES_UINT64]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %lu.",
                     value->type->typeName, result.value.ui64_result);
      break;
    case Int8:
      UA_Variant_setScalarCopy(value, &result.value.i8_result,
        &UA_TYPES[UA_TYPES_SBYTE]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %d.",
                     value->type->typeName, result.value.i8_result);
      break;
    case Int16:
      UA_Variant_setScalarCopy(value, &result.value.i16_result,
        &UA_TYPES[UA_TYPES_INT16]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %d.",
                     value->type->typeName, result.value.i16_result);
      break;
    case Int32:
      UA_Variant_setScalarCopy(value, &result.value.i32_result,
        &UA_TYPES[UA_TYPES_INT32]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %d.",
                     value->type->typeName, result.value.i32_result);
      break;
    case Int64:
      UA_Variant_setScalarCopy(value, &result.value.i64_result,
        &UA_TYPES[UA_TYPES_INT64]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %ld.",
                     value->type->typeName, result.value.i64_result);
      break;
    case Float32:
      UA_Variant_setScalarCopy(value, &result.value.f32_result,
        &UA_TYPES[UA_TYPES_FLOAT]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %f.",
                     value->type->typeName, result.value.f32_result);
      break;
    case Float64:
      UA_Variant_setScalarCopy(value, &result.value.f64_result,
        &UA_TYPES[UA_TYPES_DOUBLE]);
      iot_log_debug(uadr->lc, "Writing data of type %s with value %lf.",
                     value->type->typeName, result.value.f64_result);
      break;
    case Binary:
    default:
      iot_log_error(uadr->lc, "Type %d not supported!", result.type);
      break;
  }
  return value;
}

/* Methods checks for the addressable indicating a client is connecting */
static bool ua_is_connecting(ua_conn_addr_status *status, const char *addr_id)
{
  pthread_mutex_lock(&status->mutex);
  ua_addr *current = status->front;
  for (uint32_t i = 0; i < status->length; i++)
  {
    if (strcmp(current->addr_id, addr_id) == 0)
    {
      pthread_mutex_unlock(&status->mutex);
      return true;
    }
    if (current->next != NULL)
      current = current->next;
  }
  pthread_mutex_unlock(&status->mutex);
  return false;
}

static bool add_ua_connecting(ua_conn_addr_status *status, const char *addr_id)
{
  pthread_mutex_lock(&status->mutex);
  ua_addr *new = malloc(sizeof (ua_addr));
  memset(new, 0, sizeof(ua_addr));
  new->addr_id = strdup(addr_id);
  if (status->front == NULL)
  {
    status->front = new;
    status->back = new;
  }
  else
  {
    status->back->next = new;
    status->back = new;
  }
  status->length++;
  pthread_mutex_unlock(&status->mutex);
  return true;
}

static bool remove_ua_connecting(ua_conn_addr_status *status,
  const char *addr_id)
{
  pthread_mutex_lock(&status->mutex);
  ua_addr *current = status->front;
  ua_addr *previous = current;
  for (uint32_t i = 0; i < status->length; i++)
  {
    if (strcmp(current->addr_id, addr_id) == 0)
    {
      if (current == status->front && current == status->back)
      {
        status->front = NULL;
        status->back = NULL;
        status->length--;
        free (current->addr_id);
        free (current);
      }
      else if (current == status->front)
      {
        status->front = current->next;
        status->length--;
        free (current->addr_id);
        free (current);
      }
      else if (current == status->back)
      {
        status->back = previous;
        status->length--;
        free (current->addr_id);
        free (current);
      }
      else
      {
        previous->next = current->next;
        status->length--;
        free (current->addr_id);
        free (current);
      }
      pthread_mutex_unlock(&status->mutex);
      return true;
    }
    if (current->next != NULL)
    {
      previous = current;
      current = current->next;
    }
  }
  pthread_mutex_unlock(&status->mutex);
  return false;
}

static bool ua_connection_status(ua_conn_addr_status *connecting,
  opcua_driver *driver, opcua_connection *conn)
{

  /* Get the ua_connecting list */
  /*pthread_mutex_lock(&driver->mutex);
  ua_conn_addr_status *connecting = &driver->add_conn_status;
  pthread_mutex_unlock(&driver->mutex);*/

  UA_StatusCode retval = 0;
  if (!ua_is_connecting(connecting, conn->addr_id))
  {
    /* Check the state of the client */
    pthread_mutex_lock(&conn->mutex);
    retval = UA_Client_getState(conn->client);
    pthread_mutex_unlock(&conn->mutex);
  }

  if (retval < UA_CLIENTSTATE_SESSION &&
    !ua_is_connecting(connecting, conn->addr_id))
  {
    add_ua_connecting(connecting, conn->addr_id);
    iot_log_warning(driver->lc, "Connection id: %s is malfunctioning. Status: "
                                 "%d", conn->addr_id, retval);

    /* If the session is not active attempt to re-connect */
    conn->reconnect_count++;
    iot_log_info(driver->lc,
                  "Connection status currently %d, attempting reconnect no: %d",
                  retval, conn->reconnect_count);
    UA_Client_reset(conn->client);
    retval = opcua_connect(conn->client, conn->endpoint);
    if (retval != UA_STATUSCODE_GOOD)
    {
      iot_log_error(driver->lc, "Client failed to connect. Status Code: %s",
                     UA_StatusCode_name(retval));
      (void)remove_ua_connecting(connecting, conn->addr_id);
      return false;
    }
    else
    {
      iot_log_info(driver->lc, "Reconnect Successful. Status Code: %s",
                    UA_StatusCode_name(retval));
      (void)remove_ua_connecting(connecting, conn->addr_id);
      return true;
    }
  }
  else if (ua_is_connecting(connecting, conn->addr_id))
  {
    iot_log_warning(driver->lc,
      "A reconnect attempt is already being made for id %s", conn->addr_id);
    return false;
  }
  else
  {
    return true;
  }
}

static void dump_protocols(iot_logger_t *lc, const edgex_protocols *prots)
{
  for (const edgex_protocols *p = prots; p; p = p->next)
  {
    iot_log_debug(lc, " [%s] protocol:", p->name);
    for (const edgex_nvpairs *nv = p->properties; nv; nv = nv->next)
    {
      iot_log_debug (lc, "    %s = %s", nv->name, nv->value);
    }
  }
}

/* --- Initialize ---- */
static bool opcua_init(void *impl, struct iot_logger_t *lc,
  const edgex_nvpairs *config)
{
  opcua_driver *driver = (opcua_driver *)impl;
  driver->conn_length = 0;
  driver->lc = lc;
  pthread_mutex_init(&driver->mutex, NULL);
  pthread_mutex_init(&driver->add_conn_status.mutex, NULL);
  iot_log_info(driver->lc, "Initialising OPC-UA Device Service");
  return true;
}

/* ---- Discovery ---- */
static void opcua_discover(void *impl)
{
}

/* ---- Get ---- */
static bool opcua_get_handler(void *impl, const char *devname,
  const edgex_protocols *protocols, uint32_t nreadings,
  const edgex_device_commandrequest *requests,
  edgex_device_commandresult *readings)
{
  opcua_driver *driver = (opcua_driver *)impl;
  iot_log_debug(driver->lc, "GET on address:");
  dump_protocols(driver->lc, protocols);

  /* Find the correct opcua connection or create a new one */
  pthread_mutex_lock(&driver->mutex);
  ua_conn_addr_status *connecting = &driver->add_conn_status;
  pthread_mutex_unlock(&driver->mutex);

  opcua_connection *conn;
  if (!ua_is_connecting(connecting, devname))
  {
    add_ua_connecting(connecting, devname);
    conn = find_opcua_connection(driver, devname, (edgex_protocols *)protocols);
    (void)remove_ua_connecting(connecting, devname);
  }
  else
  {
    conn = malloc(sizeof(opcua_connection));
    memset(conn, 0, sizeof(opcua_connection));
    iot_log_debug(driver->lc,
      "A connection attempt is already in progress for id: %s",
      protocols->name);
  }

  /* Test the resulting connection, NULL if we failed to create it  */
  if (!conn)
  {
    iot_log_warning(driver->lc, "Failed to connect to Addressable: %s",
      protocols->name);
    return false;
  }
  else if (conn->client == NULL)
  {
    iot_log_warning(driver->lc, "Failed to connect to Addressable: %s",
      protocols->name);
    free(conn->addr_id);
    free(conn->endpoint);
    free(conn);
    return false;
  }
  else
  {
    /* Check the state of the client */
    pthread_mutex_lock(&conn->mutex);
    ua_conn_addr_status *status = &driver->add_conn_status;
    pthread_mutex_unlock(&conn->mutex);
    if (ua_connection_status(status, driver, conn))
    {
      iot_log_debug(driver->lc, "Get nreadings: %d", nreadings);
      for (uint32_t i = 0; i < nreadings; i++)
      {
        UA_Variant *value = UA_Variant_new();
        const UA_NodeId nodeId = get_ua_nodeid(requests[i]);
        pthread_mutex_lock(&conn->mutex);
        UA_StatusCode retval = UA_Client_readValueAttribute(conn->client,
                                                             nodeId, value);
        pthread_mutex_unlock(&conn->mutex);
        if (retval != UA_STATUSCODE_GOOD)
        {
          iot_log_warning(driver->lc,
                           "Failed to read from OPC-UA server. Status Code: %s",
                           UA_StatusCode_name(retval));
          UA_Variant_delete(value);
          return false;
        }
        readings[i] = opcua_to_edgex(value, driver);
        UA_Variant_delete(value);
      }
    }
  }
  return true;
}

/* ---- Put ---- */
static bool opcua_put_handler(void *impl, const char *devname,
    const edgex_protocols *protocols, uint32_t nvalues,
    const edgex_device_commandrequest *requests,
    const edgex_device_commandresult *values)
{
  opcua_driver *driver = (opcua_driver *)impl;
  iot_log_debug(driver->lc, "PUT on address:");
  dump_protocols(driver->lc, protocols);

  /* Find the correct opcua connection or create a new one */
  pthread_mutex_lock(&driver->mutex);
  ua_conn_addr_status *connecting = &driver->add_conn_status;
  pthread_mutex_unlock(&driver->mutex);

  opcua_connection *conn;
  if (!ua_is_connecting(connecting, devname))
  {
    add_ua_connecting(connecting, devname);
    conn = find_opcua_connection(driver, devname, (edgex_protocols *)protocols);
    (void)remove_ua_connecting(connecting, devname);
  }
  else
  {
    conn = malloc(sizeof(opcua_connection));
    memset(conn, 0, sizeof(opcua_connection));
    iot_log_debug(driver->lc,
      "A connection attempt is already in progress for id: %d",
      protocols->name);
  }

  /* Test the resulting connection, NULL if we failed to create it */
  if (!conn)
  {
    iot_log_warning(driver->lc, "Failed to connect to Addressable: %s",
      protocols->name);
    return false;
  }
  else if (conn->client == NULL)
  {
    iot_log_warning(driver->lc, "Failed to connect to Addressable: %s",
      protocols->name);
    free(conn->addr_id);
    free(conn->endpoint);
    free(conn);
    return false;
  }
  else
  {
    /* Check the state of the client */
    pthread_mutex_lock(&conn->mutex);
    ua_conn_addr_status *status = &driver->add_conn_status;
    pthread_mutex_unlock(&conn->mutex);
    if (ua_connection_status(status, driver, conn))
    {
      for (uint32_t i = 0; i < nvalues; i++)
      {
        const UA_NodeId nodeId = get_ua_nodeid(requests[i]);
        UA_Variant *value = edgex_to_opcua(values[i], driver);
        pthread_mutex_lock(&conn->mutex);
        UA_StatusCode retval = UA_Client_writeValueAttribute(conn->client,
                                                              nodeId, value);
        pthread_mutex_unlock(&conn->mutex);
        if (retval != UA_STATUSCODE_GOOD)
        {
          iot_log_warning(driver->lc, "OPCUA Write Failed. Status Code: %s",
                           UA_StatusCode_name(retval));
          UA_Variant_delete(value);
          return false;
        }
        UA_Variant_delete(value);
      }
    }
  }
  return true;
}

/* ---- Disconnect ---- */
static bool opcua_disconnect(void *impl, edgex_protocols *protocols)
{
  return true;
}

/* ---- Stop ---- */
static void opcua_stop(void *impl, bool force)
{
  opcua_driver *driver = (opcua_driver *)impl;
  iot_log_info(driver->lc, "OPCUA Device Service Stopping");
  opcua_connection *current = driver->conn_front;
  for (uint32_t i = 0; i < driver->conn_length; i++)
  {
    client_context *clientContext;
    iot_log_debug(driver->lc, "Disconnecting from: %s id: %s",
      current->endpoint, current->addr_id);
    UA_Client_disconnect(current->client);
    iot_log_debug(driver->lc, "Deleting client id: %s", current->addr_id);
    clientContext = (client_context *)UA_Client_getContext(current->client);
    free(clientContext);
    UA_Client_delete(current->client);
    driver->conn_front = current->next;
    if (driver->conn_back == current)
      driver->conn_back = NULL;
    free(current->addr_id);
    free(current->endpoint);
    free(current);
    if (driver->conn_front != NULL)
      current = driver->conn_front;
  }
  driver->conn_length = 0;
}

static void usage(void)
{
  printf("Options: \n");
  printf("   -h, --help            : Show this text\n");
  printf("   -n, --name <name>     : Set the device service name\n");
  printf("   -r, --registry <url>  : Use the registry service\n");
  printf("   -p, --profile <name>  : Set the profile name\n");
  printf("   -c, --confdir <dir>   : Set the configuration directory\n");
}

static bool testArg(int argc, char *argv[], int *pos, const char *pshort,
  const char *plong, char **var)
{
  if (strcmp(argv[*pos], pshort) == 0 || strcmp(argv[*pos], plong) == 0)
  {
    if (*pos < argc - 1)
    {
      (*pos)++;
      *var = argv[*pos];
      (*pos)++;
      return true;
    }
    else
    {
      printf("Option %s requires an argument\n", argv[*pos]);
      exit(0);
    }
  }
  char *eq = strchr(argv[*pos], '=');
  if (eq)
  {
    if (strncmp(argv[*pos], pshort, eq - argv[*pos]) == 0 ||
      strncmp(argv[*pos], plong, eq - argv[*pos]) == 0)
    {
      if (strlen(++eq))
      {
        *var = eq;
        (*pos)++;
        return true;
      }
      else
      {
        printf("Option %s requires an argument\n", argv[*pos]);
        exit(0);
      }
    }
  }
  return false;
}

int main(int argc, char *argv[])
{
  printf("   ___  ___  ___    _   _  _     ___          _          ___              _\n"
          "  / _ \\| _ \\/ __|__| | | |/_\\   |   \\ _____ _(_)__ ___  / __| ___ _ ___ _(_)__ ___\n"
          " | (_) |  _/ (_|___| |_| / _ \\  | |) / -_) V / / _/ -_) \\__ \\/ -_) '_\\ V / / _/ -_)\n"
          "  \\___/|_|  \\___|   \\___/_/ \\_\\ |___/\\___|\\_/|_\\__\\___| |___/\\___|_|  \\_/|_\\__\\___|\n\n");

  fflush(stdout);
  char *profile = "";
  char *confdir = "";
  char *service_name = "device-opcua";
  char *regURL = getenv("EDGEX_REGISTRY");
  opcua_driver *impl = malloc(sizeof (opcua_driver));
  memset(impl, 0, sizeof(opcua_driver));

  int n = 1;
  while (n < argc)
  {
    if (strcmp(argv[n], "-h") == 0 || strcmp(argv[n], "--help") == 0)
    {
      usage();
      free(impl);
      return 0;
    }
    if (testArg(argc, argv, &n, "-r", "--registry", &regURL))
    {
      continue;
    }
    if (testArg(argc, argv, &n, "-n", "--name", &service_name))
    {
      continue;
    }
    if (testArg(argc, argv, &n, "-p", "--profile", &profile))
    {
      continue;
    }
    if (testArg(argc, argv, &n, "-c", "--confdir", &confdir))
    {
      continue;
    }
    printf("Unknown option %s\n", argv[n]);
    usage();
    free(impl);
    return 0;
  }

  edgex_error e;
  e.code = 0;

  /* Device Callbacks */
  edgex_device_callbacks opcuaImpls =
    {
      opcua_init,         /* Initialize */
      opcua_discover,     /* Discovery */
      opcua_get_handler,  /* Get */
      opcua_put_handler,  /* Put */
      opcua_disconnect,   /* Disconnect */
      opcua_stop          /* Stop */
    };

  /* Initalise a new device service */
  service = edgex_device_service_new(service_name,
      VERSION, impl, opcuaImpls, &e);
  ERR_CHECK(e);

  /* Start the device service*/
  edgex_device_service_start(service, regURL, profile, confdir, &e);
  ERR_CHECK(e);

  signal(SIGINT, inthandler);
  running = true;
  while (running)
  {
    int length = 0;
    pthread_mutex_lock(&impl->mutex);
    length = impl->conn_length;
    pthread_mutex_unlock(&impl->mutex);
    if (length > 0)
    {
      pthread_mutex_lock(&impl->mutex);
      opcua_connection *current = impl->conn_front;
      pthread_mutex_unlock(&impl->mutex);
      for (uint32_t i = 0; i < length; i++)
      {
        pthread_mutex_lock(&current->mutex);
        /* Run client iterate assuming the session is active */
        UA_StatusCode retval = UA_Client_getState(current->client);
        if (retval >= UA_CLIENTSTATE_SESSION)
        {
          UA_Client_run_iterate(current->client, 0);
        }
        pthread_mutex_unlock(&current->mutex);
        if (current->next != NULL)
          current = current->next;
      }
    }

    UA_sleep_ms(500);
  }

  /* Stop the device service */
  edgex_device_service_stop(service, true, &e);
  ERR_CHECK(e);

  edgex_device_service_free(service);

  free_subs(impl->subs);
  free(impl);
  exit(0);
}
