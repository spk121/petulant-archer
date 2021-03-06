#include <glib.h>
#include <string.h>
#include "msg.h"
#include "seq.h"
#include "diag.h"
#include "tput.h"
#include "cause.h"
#include "q.h"
#include "iodir.h"
#include "packet.h"
#include "tput.h"
#include "crc.h"

static const char id_names[JZ_MSG_COUNT][23] = {
  [JZ_MSG_DATA] = "DATA",
  [JZ_MSG_RR] = "RR",
  [JZ_MSG_RNR] = "RNR",
  [JZ_MSG_CALL_REQUEST] = "CALL_REQUEST",
  [JZ_MSG_CALL_ACCEPTED] = "CALL_ACCEPTED",
  [JZ_MSG_CLEAR_REQUEST] = "CLEAR_REQUEST",
  [JZ_MSG_CLEAR_CONFIRMATION] = "CLEAR_CONFIRMATION",
  [JZ_MSG_RESET_REQUEST] = "RESET_REQUEST",
  [JZ_MSG_RESET_CONFIRMATION] = "RESET_CONFIRMATION",
  
  [JZ_MSG_CONNECT] = "CONNECT",
  [JZ_MSG_DISCONNECT] = "DISCONNECT",
  [JZ_MSG_CONNECT_INDICATION] =  "CONNECT_INDICATION",
  [JZ_MSG_DISCONNECT_INDICATION] = "DISCONNECT_INDICATION",
  [JZ_MSG_DIAGNOSTIC] = "DIAGNOSTIC",
  [JZ_MSG_DIRECTORY_REQUEST] = "DIRECTORY_REQUEST",
  [JZ_MSG_DIRECTORY] = "DIRECTORY",
  [JZ_MSG_ENQ] = "ENQ",
  [JZ_MSG_ACK] = "ACK",
  [JZ_MSG_RESTART_REQUEST] = "RESTART_REQUEST",
  [JZ_MSG_RESTART_CONFIRMATION] = "RESTART_CONFIRMATION",
};

const char *id_name(guint8 id)
{
  if (id < JZ_MSG_COUNT)
    return id_names[id];
  return "UNKNOWN";
}

gchar sixty_four_string[65] = 
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz{}";

guint8 inverse_sixty_four[128] = {
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
  0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
  0x21, 0x22, 0x23, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a,
  0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32,
  0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
  0x3b, 0x3c, 0x3d, 0x3e, 0xff, 0x3f, 0xff, 0xff
};

guint8 graphical_char_to_six_bit_integer (char C)
{
  guint8 x;
  if (C < 0 || C > 127)
    x = 0xff;
  else
    x = inverse_sixty_four[(unsigned)C];
    
  if (x == 0xFF)
    g_warning ("invalid character for 6-bit integer: %c", C);
  return x;
}

char six_bit_integer_to_graphical_char (guint8 x)
{
  if (x >= 64)
    {
      g_warning ("out of range for 6-bit integer: %d", x);
      return '?';
    }
  return sixty_four_string[x];
}

//  --------------------------------------------------------------------------
//  Validation checks

// Addresses are 1 to 16 codepoints, and must start and end
// with a non-space codepoint
gboolean address_validate (gchar *str)
{
  glong len;
  gchar *p;

  if (str == NULL)
    return FALSE;
  if (!g_utf8_validate (str, -1, NULL))
    return FALSE;
  len = g_utf8_strlen (str, -1);
  if (len == 0 || len > JZ_MSG_MAX_ADDRESS_LENGTH)
  return FALSE;
  
p = str;
for (glong i = 0; i < len; i ++)
  {
    if ((i == 0 || i == len - 1) && (!g_unichar_isgraph (g_utf8_get_char (p))))
      return FALSE;
    else if (!g_unichar_isprint (g_utf8_get_char (p)))
      return FALSE;
    p = g_utf8_next_char (p);
  }
return TRUE;
}

// Hostnames are zero to 40 codepoints, and must only contain
// graphical codepoints.
gboolean hostname_validate (gchar *str)
{
  glong len;
  gchar *p;

  if (str == NULL)
    return FALSE;
  if (!g_utf8_validate (str, -1, NULL))
    return FALSE;
  len = g_utf8_strlen (str, -1);
  if (len > JZ_MSG_MAX_HOSTNAME_LENGTH)
  return FALSE;
  
p = str;
for (glong i = 0; i < len; i ++)
  {
    if (!g_unichar_isprint (g_utf8_get_char (p)))
      return FALSE;
    p = g_utf8_next_char (p);
  }
return TRUE;
}


//  --------------------------------------------------------------------------
//  Network data encoding macros

//  Strings are encoded with 1-byte length
#define STRING_MAX  255

//  Raw blocks are encoded with a 2-byte length
#define RAW_MAX 8192

//  Put a block to the frame
#define PUT_BLOCK(host,size)                    \
  {                                             \
    memcpy (self->needle, (host), size);        \
    self->needle += size;                       \
  }

//  Get a block from the frame
#define GET_BLOCK(host,size)                    \
  {                                             \
    if (self->needle + size > self->ceiling)    \
      goto malformed;                           \
    memcpy ((host), self->needle, size);        \
    self->needle += size;                       \
  }

//  Get a 6-bit number from the frame
#define PUT_NUMBER_SIX_BIT(host)                                        \
  {                                                                     \
    *(guint8 *) self->needle = six_bit_integer_to_graphical_char (host); \
    self->needle++;                                                     \
  }

//  Put a 1-byte number to the frame
#define PUT_NUMBER1(host) {                     \
    *(guint8 *) self->needle = (host);          \
    self->needle++;                             \
  }

//  Put a 2-byte number to the frame
#define PUT_NUMBER2(host)                               \
  {                                                     \
    self->needle [0] = (guint8) (((host) >> 8)  & 255); \
    self->needle [1] = (guint8) (((host))       & 255); \
    self->needle += 2;                                  \
  }

//  Put a 4-byte number to the frame
#define PUT_NUMBER4(host)                               \
  {                                                     \
    self->needle [0] = (guint8) (((host) >> 24) & 255); \
    self->needle [1] = (guint8) (((host) >> 16) & 255); \
    self->needle [2] = (guint8) (((host) >> 8)  & 255); \
    self->needle [3] = (guint8) (((host))       & 255); \
    self->needle += 4;                                  \
  }

//  Put a 8-byte number to the frame
#define PUT_NUMBER8(host)                               \
  {                                                     \
    self->needle [0] = (guint8) (((host) >> 56) & 255); \
    self->needle [1] = (guint8) (((host) >> 48) & 255); \
    self->needle [2] = (guint8) (((host) >> 40) & 255); \
    self->needle [3] = (guint8) (((host) >> 32) & 255); \
    self->needle [4] = (guint8) (((host) >> 24) & 255); \
    self->needle [5] = (guint8) (((host) >> 16) & 255); \
    self->needle [6] = (guint8) (((host) >> 8)  & 255); \
    self->needle [7] = (guint8) (((host))       & 255); \
    self->needle += 8;                                  \
  }

//  Get a 6-bit number from the frame
#define GET_NUMBER_SIX_BIT(host)                                        \
  {                                                                     \
   if (self->needle + 1 > self->ceiling)                                \
     goto premature;                                                    \
   (host) = graphical_char_to_six_bit_integer (*(char *) self->needle); \
   self->needle++;                                                      \
   }

//  Get a 1-byte number from the frame
#define GET_NUMBER1(host) {                     \
    if (self->needle + 1 > self->ceiling)       \
      goto premature;                           \
    (host) = *(guint8 *) self->needle;          \
    self->needle++;                             \
  }

//  Get a 2-byte number from the frame
#define GET_NUMBER2(host) {                         \
    if (self->needle + 2 > self->ceiling)           \
      goto premature;                               \
    (host) = ((guint16) (self->needle [0]) << 8)    \
      +  (guint16) (self->needle [1]);              \
    self->needle += 2;                              \
  }

//  Get a 4-byte number from the frame
#define GET_NUMBER4(host) {                         \
    if (self->needle + 4 > self->ceiling)           \
      goto premature;                               \
    (host) = ((guint32) (self->needle [0]) << 24)   \
      + ((guint32) (self->needle [1]) << 16)        \
      + ((guint32) (self->needle [2]) << 8)         \
      +  (guint32) (self->needle [3]);              \
    self->needle += 4;                              \
  }

//  Get a 8-byte number from the frame
#define GET_NUMBER8(host) {                         \
    if (self->needle + 8 > self->ceiling)           \
      goto premature;                               \
    (host) = ((guint64) (self->needle [0]) << 56)   \
      + ((guint64) (self->needle [1]) << 48)        \
      + ((guint64) (self->needle [2]) << 40)        \
      + ((guint64) (self->needle [3]) << 32)        \
      + ((guint64) (self->needle [4]) << 24)        \
      + ((guint64) (self->needle [5]) << 16)        \
      + ((guint64) (self->needle [6]) << 8)         \
      +  (guint64) (self->needle [7]);              \
    self->needle += 8;                              \
  }

//  Put a string to the frame
#define PUT_STRING(host) {                      \
    string_size = strlen (host);                \
    PUT_NUMBER1 (string_size);           \
    memcpy (self->needle, (host), string_size); \
    self->needle += string_size;                \
  }

//  Get a string from the frame
#define GET_STRING(host) {                              \
    GET_NUMBER1 (string_size);                   \
    if (self->needle + string_size > (self->ceiling))   \
      goto premature;                                   \
    (host) = (char *) g_malloc (string_size + 1);       \
    memcpy ((host), self->needle, string_size);         \
    (host) [string_size] = 0;                           \
    self->needle += string_size;                        \
  }

#define RAW_SIZE(host)                          \
  (2 + (host)->len)

//  Put a raw block to the frame
#define PUT_RAW(host) {                                 \
    PUT_NUMBER2 ((host)->len);                          \
    if ((host)->len > 0)                                \
      memcpy (self->needle, (host)->data, (host)->len); \
    self->needle += (host)->len;                        \
  }

//  Get a raw block from the frame
#define GET_RAW(host)                                                   \
  {                                                                     \
    GET_NUMBER2 (string_size);                                          \
    if (self->needle + string_size > (self->ceiling))                   \
      goto premature;                                                   \
    if (string_size > RAW_MAX)                                          \
      goto malformed;                                                   \
    (host) = g_byte_array_append((host), self->needle, string_size);    \
    self->needle += string_size;                                        \
  }


void
jz_msg_free (JzMsg *M)
{
  g_free (M->hostname);
  g_free (M->directory);
  g_free (M->calling_address);
  g_free (M->called_address);
  if (M->data)
    g_byte_array_free (M->data, TRUE);
  g_free (M);
}

gsize jz_msg_binary_size (JzMsg *M)
{
  return M->packed_size;
}

gboolean
jz_msg_is_valid (JzMsg *M)
{
  if (M->valid)
    return TRUE;
  return FALSE;
}

gboolean
jz_buffer_begins_with_signature (guint8 *buf, gsize len)
{
  if (len < JZ_MSG_ENVELOPE_SIGNATURE_SIZE)
    return FALSE;
  guint32 signature;
  signature = (((guint32) (buf [0]) << 24)
               + ((guint32) (buf [1]) << 16)  
               + ((guint32) (buf [2]) << 8)   
               +  (guint32) (buf [3]));
  return (signature == JZ_MSG_SIGNATURE);
}

gboolean
jz_buffer_contains_a_message (guint8 *buf, gsize len)
{
  guint32 body_length;
  
  if (len < JZ_MSG_ENVELOPE_SIZE)
    return FALSE;
  
  if (!jz_buffer_begins_with_signature (buf, len))
    return FALSE;
  
  body_length = jz_buffer_msg_body_length (buf, len);

  if (len < JZ_MSG_ENVELOPE_SIZE + body_length + JZ_MSG_PADDING_LENGTH(body_length))
    return FALSE;
    
  return TRUE;
}

gsize
jz_buffer_msg_binary_size (guint8 *buf, gsize len)
{
  guint32 body_length;
  
  g_assert (len >= JZ_MSG_ENVELOPE_SIZE);
  g_assert (jz_buffer_begins_with_signature (buf, len));

  body_length = jz_buffer_msg_body_length (buf, len);
  
  g_assert (body_length <= JZ_MSG_MAX_PAYLOAD_LENGTH);

  return JZ_MSG_ENVELOPE_SIZE + body_length + JZ_MSG_PADDING_LENGTH(body_length);
}

gboolean
jz_buffer_msg_envelope_is_valid (guint8 *buf, gsize len)
{
  guint32 body_length, crc, padding;
  
  if (len < JZ_MSG_ENVELOPE_SIZE)
    return FALSE;

  if (!jz_buffer_begins_with_signature (buf, len))
    return FALSE;

  body_length = jz_buffer_msg_body_length (buf, len);

  if (body_length > JZ_MSG_MAX_PAYLOAD_LENGTH)
    return FALSE;
  
  padding = JZ_MSG_PADDING_LENGTH(body_length);

  unsigned int crc_measured = digital_crc32(buf + JZ_MSG_ENVELOPE_HEADER_SIZE, body_length);

  crc = (((guint32) (buf [JZ_MSG_ENVELOPE_HEADER_SIZE + body_length + padding]) << 24)
         + ((guint32) (buf [JZ_MSG_ENVELOPE_HEADER_SIZE + body_length + padding + 1]) << 16)  
         + ((guint32) (buf [JZ_MSG_ENVELOPE_HEADER_SIZE + body_length + padding + 2]) << 8)   
         +  (guint32) (buf [JZ_MSG_ENVELOPE_HEADER_SIZE + body_length + padding + 3]));

  if (crc != crc_measured)
    return FALSE;
  
  return TRUE;  
}

guint32
jz_buffer_msg_body_length (guint8 *buf, gsize len)
{
  guint32 body_length;
  
  g_assert (len >= JZ_MSG_ENVELOPE_HEADER_SIZE);

  body_length = (((guint32) (buf [4]) << 24)
                 + ((guint32) (buf [5]) << 16)  
                 + ((guint32) (buf [6]) << 8)   
                 +  (guint32) (buf [7]));

  return body_length;
}

guint16
jz_buffer_msg_lcn (guint8 *buf, gsize len)
{
  guint16 lcn;
  
  g_assert (len >= JZ_MSG_ENVELOPE_HEADER_SIZE + JZ_MSG_PAYLOAD_HEADER_SIZE);

  lcn = (((guint32) (buf [JZ_MSG_ENVELOPE_HEADER_SIZE + 2]) << 8)
                 +  (guint32) (buf [JZ_MSG_ENVELOPE_HEADER_SIZE + 3]));

  return lcn;
}


// Given a raw data buffer BUF of size LEN.
// SIZ is the number of bytes that were parsed, or zero if
//   there isn't enough information yet to parse.
// MSG is the unpacked message, or NULL if no message was unpacked.
// If IS_FATAL is true, this buffer is hopelessly corrupt
// IF IS_ERROR is true, a bad message was skipped and MSG contains
// a diagnostic message.

void
jz_buffer_parse_into_message (guint8 *buf, gsize len, gsize *siz, JzMsg **msg,
                              gboolean *is_error, gboolean *is_fatal)
{
  *msg = NULL;
  *siz = 0;
  *is_error = FALSE;
  *is_fatal = FALSE;

  g_assert (len > 0);

  // The smallest possible message is 12 bytes, which is just an envelope
  if (len < JZ_MSG_ENVELOPE_SIZE)
    {
      // We haven't even received enough data for a message envelope.
      // Do nothing, for now
    }
  // A message is supposed to start with '~SVC'
  else if (!jz_buffer_begins_with_signature (buf, len))
    {
      // The message envelope didn't start with the right string.
      // We're hopelessly confused now and can't recover.
      *is_fatal = TRUE;
    }
  else if (!jz_buffer_contains_a_message (buf, len))
    {
      // We haven't received enough data yet for this message.  Do
      // nothing, for now.
    }
  else if (!jz_buffer_msg_envelope_is_valid (buf, len))
    {
      // Bad envelope usually means bad checksum.
      // FIXME: extract version, ID, and lcn for the following call
      *msg = jz_msg_new_diagnostic (d_invalid_envelope, 0, 0, 0);
      *is_error = TRUE;
      
      // Don't unpack this message.  Just advance to the next message.
      *siz = jz_buffer_msg_binary_size (buf, len);
      
    }
  else if (jz_buffer_msg_body_length (buf, len) < JZ_MSG_PAYLOAD_HEADER_SIZE)
    {
      // Check that the message body at least contains a complete header
      // FIXME: extract version, ID, and LCN for the following call
      *msg = jz_msg_new_diagnostic (d_packet_too_short, 0, 0, 0);
      *is_error = TRUE;
      
      // Don't unpack this message.  Just advance to the next message.
      *siz = jz_buffer_msg_binary_size (buf, len);
    }
  else if (jz_buffer_msg_lcn (buf, len) > JZ_MSG_MAX_LCN)
    {
      // This LCN is out of range for this client
      // FIXME: extract version, ID, and LCN for the following call
      *msg = jz_msg_new_diagnostic (d_packet_on_unassigned_logical_channel, 0, 0, 0);
      *is_error = TRUE;
      
      // Don't unpack this message.  Just advance to the next
      // message.
      *siz = jz_buffer_msg_binary_size (buf, len);
    }
  else
    {
      // If we get this far, it is safe to attempt to unpack the
      // message payload
      *msg = jz_msg_new_from_data (buf, len);
      *siz = jz_msg_binary_size (*msg);
      
      if (!(*msg)->valid)
        {
          // The message payload appears to be too short or too
          // long.
          jz_msg_free (*msg);
          
          *msg = jz_msg_new_diagnostic ((*msg)->invalidity, (*msg)->version, (*msg)->id, (*msg)->lcn);
          *is_error = TRUE;
        }
    }
}


JzMsg *
jz_msg_new_from_data (gpointer buf, gsize len)
{
  g_assert (buf);
  JzMsg *self = g_new0(JzMsg, 1);
  self->data = g_byte_array_new ();
  size_t string_size;
  size_t frame_length;
  
  g_assert (len > JZ_MSG_ENVELOPE_SIZE);
  
  //  Get and check protocol signature
  self->needle = buf;

  // Initially we don't know enough to properly set the ceiling
  self->ceiling = buf + len;
  
  GET_NUMBER4 (self->signature);
  g_assert (self->signature == JZ_MSG_SIGNATURE);
  //  Get message id and parse per message type
  GET_NUMBER4 (frame_length);

  // Now that we have a frame size, we can properly set the ceiling
  guint32 padding = JZ_MSG_PADDING_LENGTH (frame_length);
  self->ceiling = buf + JZ_MSG_ENVELOPE_SIZE + frame_length + padding;
  
  GET_NUMBER1 (self->version);
  GET_NUMBER1 (self->id);
  GET_NUMBER2 (self->lcn);
  
  if (self->version != 1)
    {
      self->invalidity = d_unknown_version;
      goto malformed;
    }
  
  switch (self->id)
    {
    case JZ_MSG_DATA:
      GET_NUMBER1 (self->q);
      GET_NUMBER2 (self->pr);
      GET_NUMBER2 (self->ps);
      g_byte_array_set_size (self->data, 0);
      GET_RAW (self->data);
      break;
      
    case JZ_MSG_RR:
      GET_NUMBER2 (self->pr);
      break;
      
    case JZ_MSG_RNR:
      GET_NUMBER2 (self->pr);
      break;
      
    case JZ_MSG_CALL_REQUEST:
      // Address block
      g_free (self->calling_address);
      GET_STRING (self->calling_address);
      g_free (self->called_address);
      GET_STRING (self->called_address);

      // Facilities
      GET_NUMBER1 (self->packet);
      GET_NUMBER2 (self->window);
      GET_NUMBER1 (self->throughput);

      // User data
      g_byte_array_set_size (self->data, 0);
      GET_RAW (self->data);
      break;
      
    case JZ_MSG_CALL_ACCEPTED:
      // Address block
      g_free (self->calling_address);
      GET_STRING (self->calling_address);
      g_free (self->called_address);
      GET_STRING (self->called_address);

      // Facilities
      GET_NUMBER1 (self->packet);
      GET_NUMBER2 (self->window);
      GET_NUMBER1 (self->throughput);

      // User data
      g_byte_array_set_size (self->data, 0);
      GET_RAW (self->data);
      break;
      
    case JZ_MSG_CLEAR_REQUEST:
      GET_NUMBER1 (self->cause);
      GET_NUMBER1 (self->diagnostic);
      break;
      
    case JZ_MSG_CLEAR_CONFIRMATION:
      break;
      
    case JZ_MSG_RESET_REQUEST:
      GET_NUMBER1 (self->cause);
      GET_NUMBER1 (self->diagnostic);
      break;
      
    case JZ_MSG_RESET_CONFIRMATION:
      break;
      
    case JZ_MSG_CONNECT:
      g_free (self->calling_address);
      GET_STRING (self->calling_address);
      GET_NUMBER1 (self->iodir);
      break;
      
    case JZ_MSG_CONNECT_INDICATION:
      break;
      
    case JZ_MSG_DISCONNECT:
      break;
      
    case JZ_MSG_DISCONNECT_INDICATION:
      break;
      
    case JZ_MSG_DIAGNOSTIC:
      GET_NUMBER1 (self->diagnostic);
      GET_NUMBER1 (self->diagnostic_version);
      GET_NUMBER1 (self->diagnostic_id);
      GET_NUMBER2 (self->diagnostic_lcn);
      break;
      
    case JZ_MSG_DIRECTORY_REQUEST:
      break;
      
    case JZ_MSG_DIRECTORY:
#if 0
      GET_NUMBER1 (hash_size);
      self->workers = zhash_new ();
      zhash_autofree (self->workers);
      while (hash_size--) {
        char *string;
        GET_STRING (string);
        char *value = strchr (string, '=');
        if (value)
          *value++ = 0;
        zhash_insert (self->workers, string, value);
        free (string);
      }
#endif
      break;
      
    case JZ_MSG_ENQ:
      break;
      
    case JZ_MSG_ACK:
      break;
      
    case JZ_MSG_RESTART_REQUEST:
      GET_NUMBER1 (self->cause);
      GET_NUMBER1 (self->diagnostic);
      break;
      
    case JZ_MSG_RESTART_CONFIRMATION:
      break;
    default:
      self->invalidity = d_unknown_message_type;
      goto malformed;
    }

  for (int i = 0; i < padding; i ++)
    {
      guint8 padding;
      GET_NUMBER1 (padding);
    }

  GET_NUMBER4 (self->crc);
  //  Successful unpacking
  self->packed_size = self->needle - (guint8 *) buf;

  // If the message is shorter than expected
  if (self->packed_size < JZ_MSG_ENVELOPE_SIZE + frame_length + padding)
    {
      g_critical ("message payload smaller than envelope '%s'\n", id_name (self->id));
      self->valid = FALSE;
      self->invalidity = d_packet_too_long;
    }
  else
    self->valid = TRUE;
  return self;
    
  //  Error returns
 malformed:
  g_critical ("malformed message '%s'\n", id_name (self->id));
  self->valid = FALSE;
  self->packed_size = self->needle - (guint8 *) buf;
  return self;
  
 premature:
  g_critical ("message payload too big for envelope '%s'\n", id_name (self->id));
  self->invalidity = d_packet_too_short;
  self->packed_size = self->needle - (guint8 *) buf;
  return self;
}

GByteArray *
jz_msg_to_byte_array (JzMsg *self)
{
  //  Calculate size of serialized data
  size_t frame_size = JZ_MSG_PAYLOAD_HEADER_SIZE;                   //  Header: version + ID + LCN
  switch (self->id) {
  case JZ_MSG_DATA:
    //  q is a 1-byte integer
    frame_size += 1;
    //  pr is a 2-byte integer
    frame_size += 2;
    //  ps is a 2-byte integer
    frame_size += 2;
    //  Raw data
    frame_size += RAW_SIZE (self->data);
    break;

  case JZ_MSG_RR:
    //  pr is a 2-byte integer
    frame_size += 2;
    break;

  case JZ_MSG_RNR:
    //  pr is a 2-byte integer
    frame_size += 2;
    break;

  case JZ_MSG_CALL_REQUEST:
    //  calling_address is a string with 1-byte length
    frame_size++;       //  Size is one octet
    if (self->calling_address)
      frame_size += strlen (self->calling_address);
    //  called_address is a string with 1-byte length
    frame_size++;       //  Size is one octet
    if (self->called_address)
      frame_size += strlen (self->called_address);
    //  packet is a 1-byte integer
    frame_size += 1;
    //  window is a 2-byte integer
    frame_size += 2;
    //  throughput is a 1-byte integer
    frame_size += 1;
    //  Raw data
    frame_size += RAW_SIZE (self->data);
    break;

  case JZ_MSG_CALL_ACCEPTED:
    //  calling_address is a string with 1-byte length
    frame_size++;       //  Size is one octet
    if (self->calling_address)
      frame_size += strlen (self->calling_address);
    //  called_address is a string with 1-byte length
    frame_size++;       //  Size is one octet
    if (self->called_address)
      frame_size += strlen (self->called_address);
    //  packet is a 1-byte integer
    frame_size += 1;
    //  window is a 2-byte integer
    frame_size += 2;
    //  throughput is a 1-byte integer
    frame_size += 1;
    //  Raw data
    frame_size += RAW_SIZE (self->data);
    break;

  case JZ_MSG_CLEAR_REQUEST:
    //  cause is a 1-byte integer
    frame_size += 1;
    //  diagnostic is a 1-byte integer
    frame_size += 1;
    break;

  case JZ_MSG_CLEAR_CONFIRMATION:
    break;

  case JZ_MSG_RESET_REQUEST:
    //  cause is a 1-byte integer
    frame_size += 1;
    //  diagnostic is a 1-byte integer
    frame_size += 1;
    break;

  case JZ_MSG_RESET_CONFIRMATION:
    break;

  case JZ_MSG_CONNECT:
    //  calling_address is a string with 1-byte length
    frame_size++;       //  Size is one octet
    if (self->calling_address)
      frame_size += strlen (self->calling_address);
    //  iodir is a 1-byte integer
    frame_size += 1;
    break;

  case JZ_MSG_CONNECT_INDICATION:
    break;

  case JZ_MSG_DISCONNECT:
    break;

  case JZ_MSG_DISCONNECT_INDICATION:
    break;

  case JZ_MSG_DIAGNOSTIC:
    //  diagnostic is a 1-byte integer
    frame_size += 1;
    //  diagnostic_version is a 1-byte integer
    frame_size += 1;
    //  diagnostic_id is a 1-byte integer
    frame_size += 1;
    //  diagnostic_lcn is a 2-byte integer
    frame_size += 2;
    break;

  case JZ_MSG_DIRECTORY_REQUEST:
    break;

#if 0
  case JZ_MSG_DIRECTORY:
    //  workers is an array of key=value strings
    frame_size++;       //  Size is one octet
    if (self->workers) {
      self->workers_bytes = 0;
      //  Add up size of dictionary contents
      zhash_foreach (self->workers, s_workers_count, self);
    }
    frame_size += self->workers_bytes;
    break;
#endif
        
  case JZ_MSG_ENQ:
    break;

  case JZ_MSG_ACK:
    break;

  case JZ_MSG_RESTART_REQUEST:
    //  cause is a 1-byte integer
    frame_size += 1;
    //  diagnostic is a 1-byte integer
    frame_size += 1;
    break;

  case JZ_MSG_RESTART_CONFIRMATION:
    break;

  default:
    g_error ("bad message type '%s'", id_name (self->id));
  }

  size_t padding = JZ_MSG_PADDING_LENGTH(frame_size);
  
  //  Now serialize message into the message
  GByteArray *buf = g_byte_array_sized_new (JZ_MSG_ENVELOPE_SIZE + frame_size + padding);
  buf->len = JZ_MSG_ENVELOPE_SIZE + frame_size + padding;
  self->needle = buf->data;
  size_t string_size;
  PUT_NUMBER4 (self->signature);
  PUT_NUMBER4 (frame_size);
  PUT_NUMBER1 (self->version);
  PUT_NUMBER1 (self->id);
  PUT_NUMBER2 (self->lcn);

  switch (self->id) {
  case JZ_MSG_DATA:
    PUT_NUMBER1 (self->q);
    PUT_NUMBER2 (self->pr);
    PUT_NUMBER2 (self->ps);
    PUT_RAW (self->data);
    break;

  case JZ_MSG_RR:
    PUT_NUMBER2 (self->pr);
    break;

  case JZ_MSG_RNR:
    PUT_NUMBER2 (self->pr);
    break;

  case JZ_MSG_CALL_REQUEST:
    if (self->calling_address) {
      PUT_STRING (self->calling_address);
    } else
      PUT_NUMBER1 (0);    //  Empty string
    if (self->called_address) {
      PUT_STRING (self->called_address);
    } else
      PUT_NUMBER1 (0);    //  Empty string
    PUT_NUMBER1 (self->packet);
    PUT_NUMBER2 (self->window);
    PUT_NUMBER1 (self->throughput);
    PUT_RAW (self->data);
    break;

  case JZ_MSG_CALL_ACCEPTED:
    if (self->calling_address) {
      PUT_STRING (self->calling_address);
    } else
      PUT_NUMBER1 (0);    //  Empty string
    if (self->called_address) {
      PUT_STRING (self->called_address);
    } else
      PUT_NUMBER1 (0);    //  Empty string
    PUT_NUMBER1 (self->packet);
    PUT_NUMBER2 (self->window);
    PUT_NUMBER1 (self->throughput);
    PUT_RAW (self->data);
    break;

  case JZ_MSG_CLEAR_REQUEST:
    PUT_NUMBER1 (self->cause);
    PUT_NUMBER1 (self->diagnostic);
    break;

  case JZ_MSG_CLEAR_CONFIRMATION:
    break;

  case JZ_MSG_RESET_REQUEST:
    PUT_NUMBER1 (self->cause);
    PUT_NUMBER1 (self->diagnostic);
    break;

  case JZ_MSG_RESET_CONFIRMATION:
    break;

  case JZ_MSG_CONNECT:
    if (self->calling_address) {
      PUT_STRING (self->calling_address);
    } else
      PUT_NUMBER1 (0);    //  Empty string
    PUT_NUMBER1 (self->iodir);
    break;

  case JZ_MSG_CONNECT_INDICATION:
    break;

  case JZ_MSG_DISCONNECT:
    break;

  case JZ_MSG_DISCONNECT_INDICATION:
    break;

  case JZ_MSG_DIAGNOSTIC:
    PUT_NUMBER1 (self->diagnostic);
    PUT_NUMBER1 (self->diagnostic_version);
    PUT_NUMBER1 (self->diagnostic_id);
    PUT_NUMBER2 (self->diagnostic_lcn);
    break;

  case JZ_MSG_DIRECTORY_REQUEST:
    break;

#if 0
  case JZ_MSG_DIRECTORY:
    if (self->workers != NULL) {
      PUT_NUMBER1 (zhash_size (self->workers));
      zhash_foreach (self->workers, s_workers_write, self);
    } else
      PUT_NUMBER1 (0);    //  Empty dictionary
    break;
#endif
        
  case JZ_MSG_ENQ:
    break;

  case JZ_MSG_ACK:
    break;

  case JZ_MSG_RESTART_REQUEST:
    PUT_NUMBER1 (self->cause);
    PUT_NUMBER1 (self->diagnostic);
    break;

  case JZ_MSG_RESTART_CONFIRMATION:
    break;
  }

  for (int i = 0; i < padding; i ++)
    PUT_NUMBER1 (0);
  self->crc = digital_crc32(buf->data + JZ_MSG_ENVELOPE_HEADER_SIZE, frame_size);
  PUT_NUMBER4 (self->crc);
  return buf;
}

diag_t
jz_msg_validate (JzMsg *self)
{
  g_assert (self);
  
  if (self->signature != JZ_MSG_SIGNATURE)
    return d_invalid_signature;
  else if (self->version != 1)
    return d_unknown_version;
  else if (self->id < 0 || self->id >= JZ_MSG_COUNT)
    return d_unknown_message_type;
#if 0
  // Try to keep this alphabetized
  else if (self->id == JZ_MSG_CALL_ACCEPTED
           || self->id == JZ_MSG_CALL_REQUEST
           || self->id == JZ_MSG_CLEAR_REQUEST)
    {
      if (self->lcn <= 0 || self->lcn > JZ_MSG_MAX_LCN)
        return d_invalid_lcn;
    }
  else if (self->id == JZ_MSG_CALL_ACCEPTED
           || self->id == JZ_MSG_CALL_REQUEST
           )
    {
      if (packet_rngchk (self->packet) < 0)
        return d_invalid_packet_facility;
      else if (packet_rngchk (self->packet) > 0)
        return d_invalid_packet_facility;

      else if (tput_rngchk (self->throughput) < 0)
        return d_invalid_throughput_facility;
      else if (tput_rngchk (self->throughput) > 0)
        return d_invalid_throughput_facility;

      else if (window_rngchk (self->window) < 0)
        return d_invalid_window_facility;
      else if (window_rngchk (self->window) > 0)
        return d_invalid_window_facility;
    }
  else if (self->id == JZ_MSG_CALL_ACCEPTED
           || self->id == JZ_MSG_CALL_REQUEST
           )
    {
      if (!address_validate(self->called_address))
        return d_invalid_called_address;
    }
  
  else if  (self->id == JZ_MSG_CALL_ACCEPTED
            || self->id == JZ_MSG_CALL_REQUEST
            || self->id == JZ_MSG_CLEAR_CONFIRMATION
            || self->id == JZ_MSG_CONNECT
            )
    {
      if (!address_validate(self->calling_address))
        return d_invalid_calling_address;
    }
  
  else if (self->id == JZ_MSG_CALL_ACCEPTED
           || self->id == JZ_MSG_CALL_REQUEST)
    {
      // Zero-length data frames are OK!!
      if (self->data->len > JZ_MSG_MAX_CALL_REQUEST_DATA_SIZE)
        return d_data_too_long;
    }

  else if (self->id == JZ_MSG_DATA)
    {
      // zero-length data frames are not allowed!!
      if (self->data->len == 0)
        return d_data_too_short;
      else if (self->data->len > JZ_MSG_MAX_DATA_SIZE)
        return d_data_too_long;
    }
  
  else if (self->id == JZ_MSG_CLEAR_REQUEST
           || self->id == JZ_MSG_DIAGNOSTIC
           || self->id == JZ_MSG_RESET_REQUEST)
    {
      if (!cause_validate (self->cause))
        return d_invalid_cause;
      if (!diag_validate (self->diagnostic))
        return d_invalid_diagnostic;
    }

  else if (self->id == JZ_MSG_DATA)
    {
      if (!q_validate (self->q))
        return d_invalid_q;
      else if (seq_rngchk (self->ps) != 0)
        return d_invalid_ps;
    }
  
  else if (self->id == JZ_MSG_DATA
           || self->id == JZ_MSG_RR
           || self->id == JZ_MSG_RNR)
    {
      if (seq_rngchk (self->pr) != 0)
        return d_invalid_pr;
    }
  
  else if (self->id == JZ_MSG_CONNECT)
    {
      if (!iodir_validate (self->iodir))
        return d_invalid_directionality_facility;
      
      else if (self->hostname != NULL)
        {
          if (!g_utf8_validate (self->hostname, -1, NULL)
              || g_utf8_strlen (self->hostname, -1) > JZ_MSG_MAX_HOSTNAME_LENGTH)
            return d_invalid_hostname;
        }
    }
#if 0
 case JZ_MSG_DIRECTORY:
   GET_NUMBER1 (hash_size);
   self->workers = zhash_new ();
   zhash_autofree (self->workers);
   while (hash_size--) {
     char *string;
     GET_STRING (string);
     char *value = strchr (string, '=');
     if (value)
       *value++ = 0;
     zhash_insert (self->workers, string, value);
     free (string);
   }
#endif
#endif
   return d_ok;
}

#define MSG_HEADER(__msg)                       \
  (__msg)->signature = JZ_MSG_SIGNATURE;        \
  (__msg)->version = 1;                         \
  (__msg)->valid = TRUE;

#define MSG_ERROR_CHECK(__msg)                                  \
  {                                                             \
    diag_t __err;                                               \
    if (((__err) = jz_msg_validate (__msg)) != d_ok)            \
      {                                                         \
        jz_msg_free (__msg);                                    \
        g_error ("tried to create invalid %s msg, error %s",    \
                 id_name(__msg->id), diag_name(__err));      \
      }                                                         \
  }

JzMsg *
jz_msg_new_connect (gchar *calling_address, guint8 iodir)
{
  JzMsg *self = g_new0(JzMsg, 1);
    
  MSG_HEADER(self);
  self->id = JZ_MSG_CONNECT;
  self->calling_address = g_strdup (calling_address);
  self->iodir = iodir;
  MSG_ERROR_CHECK(self);
  return self;
}

JzMsg *
jz_msg_new_connect_indication ()
{
  JzMsg *self = g_new0(JzMsg, 1);
    
  MSG_HEADER(self);
  self->id = JZ_MSG_CONNECT_INDICATION;
  MSG_ERROR_CHECK(self);
  return self;
}

JzMsg *
jz_msg_new_disconnect ()
{
  JzMsg *self = g_new0(JzMsg, 1);
    
  MSG_HEADER(self);
  self->id = JZ_MSG_DISCONNECT;
  MSG_ERROR_CHECK(self);
  return self;
}

JzMsg *
jz_msg_new_disconnect_indication ()
{
  JzMsg *self = g_new0(JzMsg, 1);
    
  MSG_HEADER(self);
  self->id = JZ_MSG_DISCONNECT_INDICATION;
  MSG_ERROR_CHECK(self);
  return self;
}


 JzMsg *
jz_msg_new_call_request (guint16 lcn, gchar *calling_address, gchar *called_address, packet_t packet,
                         guint16 window, tput_t throughput, guint8 *data, gsize len)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER (self);
  self->id = JZ_MSG_CALL_REQUEST;
  self->lcn = lcn;
  self->calling_address = g_strdup (calling_address);
  self->called_address = g_strdup (called_address);
  self->packet = packet;
  self->window = window;
  self->throughput = throughput;
  self->data = g_byte_array_new_take (data, len);
  MSG_ERROR_CHECK(self);
  return self;
}

 
JzMsg *
jz_msg_new_call_accepted (guint16 lcn, gchar *calling_address, gchar *called_address, packet_t packet,
						  guint16 window, tput_t throughput, guint8 *data, gsize len)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER (self);
  self->id = JZ_MSG_CALL_ACCEPTED;
  self->lcn = lcn;
  self->calling_address = g_strdup (calling_address);
  self->called_address = g_strdup (called_address);
  self->packet = packet;
  self->window = window;
  self->throughput = throughput;
  self->data = g_byte_array_new_take (data, len);
  MSG_ERROR_CHECK(self);
  return self;
}

JzMsg *jz_msg_new_clear_request (guint16 lcn, clear_cause_t cause, diag_t diagnostic)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER (self);
  self->id = JZ_MSG_CLEAR_REQUEST;
  self->lcn = lcn;
  self->cause = cause;
  self->diagnostic = diagnostic;
  MSG_ERROR_CHECK(self);
  return self;
}

JzMsg *jz_msg_new_clear_confirmation (guint16 lcn)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER (self);
  self->id = JZ_MSG_CLEAR_CONFIRMATION;
  self->lcn = lcn;
  MSG_ERROR_CHECK(self);
  return self;
}
 
JzMsg *
jz_msg_new_data (guint16 lcn, guint8 q, guint16 pr, guint16 ps, guint8 *data, gsize len)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER(self);
  self->id = JZ_MSG_DATA;
  self->lcn = lcn;
  self->q = q;
  self->pr = pr;
  self->ps = ps;
  self->data = g_byte_array_new_take (data, len);
  MSG_ERROR_CHECK(self);
  g_byte_array_ref (self->data);
  return self;
}

JzMsg *jz_msg_new_rr (guint16 lcn, guint16 pr)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER(self);
  self->id = JZ_MSG_RR;
  self->lcn = lcn;
  self->pr = pr;
  return self;
}

JzMsg *jz_msg_new_rnr (guint16 lcn, guint16 pr)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER(self);
  self->id = JZ_MSG_RNR;
  self->lcn = lcn;
  self->pr = pr;
  return self;
}

JzMsg *jz_msg_new_reset_request (guint16 lcn, reset_cause_t cause, diag_t diagnostic)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER(self);
  self->id = JZ_MSG_RESET_REQUEST;
  self->lcn = lcn;
  self->cause = cause;
  self->diagnostic = diagnostic;
  MSG_ERROR_CHECK(self);
  return self;
}

JzMsg *jz_msg_new_reset_confirmation (guint16 lcn)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER(self);
  self->id = JZ_MSG_RESET_CONFIRMATION;
  self->lcn = lcn;
  MSG_ERROR_CHECK(self);
  return self;
}
 
 
JzMsg *
jz_msg_new_diagnostic (diag_t diagnostic, guint8 version, guint8 id, guint16 lcn)
{
  JzMsg *self = g_new0(JzMsg, 1);
    
  MSG_HEADER(self);
  self->id = JZ_MSG_DIAGNOSTIC;
  self->diagnostic = diagnostic;
  self->diagnostic_version = version;
  self->diagnostic_id = id;
  self->diagnostic_lcn = lcn;
  MSG_ERROR_CHECK(self);
  return self;
}

JzMsg *jz_msg_new_restart_request (restart_cause_t cause, diag_t diagnostic)
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER (self);
  self->id = JZ_MSG_RESTART_REQUEST;
  self->cause = cause;
  self->diagnostic = diagnostic;
  MSG_ERROR_CHECK(self);
  return self;
}

JzMsg *jz_msg_new_restart_confirmation ()
{
  JzMsg *self = g_new0(JzMsg, 1);

  MSG_HEADER (self);
  self->id = JZ_MSG_RESTART_CONFIRMATION;
  MSG_ERROR_CHECK(self);
  return self;
}


////////////////////////////////////////////////////////////////

gboolean
jz_msg_is_for_channel (JzMsg *M)
{
  if ((M->lcn >= 1 && M->lcn <= JZ_MSG_MAX_LCN)
      && (M->id == JZ_MSG_CALL_REQUEST
          || M->id == JZ_MSG_DATA
          || M->id == JZ_MSG_RR
          || M->id == JZ_MSG_RNR
          || M->id == JZ_MSG_CALL_ACCEPTED
          || M->id == JZ_MSG_CLEAR_REQUEST
          || M->id == JZ_MSG_CLEAR_CONFIRMATION
          || M->id == JZ_MSG_RESET_REQUEST
          || M->id == JZ_MSG_RESET_CONFIRMATION))
    return TRUE;
  return FALSE;
}



