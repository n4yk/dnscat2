#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "select_group.h"

#include "driver.h"
#include "driver_tcp.h"
#include "memory.h"
#include "packet.h"
#include "session.h"
#include "time.h"
#include "types.h"

session_t *session_create(driver_t *driver)
{
  session_t *session     = (session_t*)safe_malloc(sizeof(session_t));
  session->id            = rand() & 0xFFFF;
  session->state         = SESSION_STATE_NEW;
  session->their_seq     = 0;
  session->my_seq        = rand() & 0xFFFF;
  session->stdin_closed  = FALSE;
  session->driver        = driver;

  session->incoming_data = buffer_create(BO_BIG_ENDIAN);
  session->outgoing_data = buffer_create(BO_BIG_ENDIAN);

  return session;
}

void session_destroy(session_t *session)
{
  buffer_destroy(session->incoming_data);
  buffer_destroy(session->outgoing_data);
  safe_free(session);
}

void session_send(session_t *session, uint8_t *data, size_t length)
{
  buffer_add_bytes(session->outgoing_data, data, length);
}

void session_recv(session_t *session, uint8_t *data, size_t length)
{
  buffer_add_bytes(session->incoming_data, data, length);
}

NBBOOL session_is_data_queued(session_t *session)
{
  return buffer_get_remaining_bytes(session->outgoing_data) != 0;
}

static void clean_up_buffers(session_t *session)
{
  if(buffer_get_remaining_bytes(session->outgoing_data) == 0)
    buffer_clear(session->outgoing_data);
  if(buffer_get_remaining_bytes(session->incoming_data) == 0)
    buffer_clear(session->incoming_data);
}

static void do_recv_stuff(session_t *session)
{
  packet_t *packet = driver_recv_packet(session->driver);

  if(packet)
  {
    switch(session->state)
    {
      case SESSION_STATE_NEW:
        if(packet->message_type == MESSAGE_TYPE_SYN)
        {
          printf("[[dnscat]] SYN received from server (SEQ = 0x%04x)\n", packet->body.syn.seq);
          session->their_seq = packet->body.syn.seq;
          session->state = SESSION_STATE_ESTABLISHED;
        }
        else if(packet->message_type == MESSAGE_TYPE_MSG)
        {
          printf("[[WARNING]] :: Unexpected MSG received (ignoring)\n");
        }
        else if(packet->message_type == MESSAGE_TYPE_FIN)
        {
          printf("[[dnscat]] :: Connection terminated by server\n");
          exit(0);
        }
        else
        {
          printf("[[ERROR]] :: Unknown packet type: 0x%02x\n", packet->message_type);
          exit(1);
        }

        break;
      case SESSION_STATE_ESTABLISHED:
        if(packet->message_type == MESSAGE_TYPE_SYN)
        {
          printf("[[WARNING]] :: Unexpected SYN received (ignoring)\n");
        }
        else if(packet->message_type == MESSAGE_TYPE_MSG)
        {
          printf("[[dnscat]] :: Received a MSG from the server\n");

          /* Validate the SEQ */
          if(packet->body.msg.seq == session->their_seq)
          {
            /* Verify the ACK is sane */
            if(packet->body.msg.ack <= session->my_seq + buffer_get_remaining_bytes(session->outgoing_data))
            {
              /* Increment their sequence number */
              session->their_seq += packet->body.msg.data_length;

              /* Remove the acknowledged data from the buffer */
              buffer_consume(session->outgoing_data, packet->body.msg.ack - session->my_seq);

              /* Increment my sequence number */
              session->my_seq = packet->body.msg.ack;

              /* Print the data, if we received any */
              if(packet->body.msg.data_length > 0)
                printf("[[data]] :: %s [0x%zx bytes]\n", packet->body.msg.data, packet->body.msg.data_length);
            }
            else
            {
              printf("[[WARNING]] :: Bad ACK received\n");
            }
          }
          else
          {
            printf("[[WARNING]] :: Bad SEQ received\n");
          }
        }
        else if(packet->message_type == MESSAGE_TYPE_FIN)
        {
          printf("[[dnscat]] :: Connection terminated by server\n");
          exit(0);
        }
        else
        {
          printf("[[ERROR]] :: Unknown packet type: 0x%02x\n", packet->message_type);
          exit(1);
        }

        break;
      default:
        printf("[[ERROR]] :: Wound up in an unknown state: 0x%x\n", session->state);
        exit(1);
    }
  }
}

static void do_send_stuff(session_t *session)
{
  packet_t *packet;
  uint8_t  *data;
  size_t    length;

  switch(session->state)
  {
    case SESSION_STATE_NEW:
      printf("[[dnscat]] :: Sending a SYN packet (SEQ = 0x%04x)...\n", session->my_seq);
      packet = packet_create_syn(session->id, session->my_seq, 0);
      driver_send_packet(session->driver, packet);
      packet_destroy(packet);
      break;

    case SESSION_STATE_ESTABLISHED:
      /* Read data without consuming it (ie, leave it in the buffer till it's ACKed) */
      data = buffer_read_remaining_bytes(session->outgoing_data, &length, session->driver->max_packet_size - 8, FALSE); /* TODO: Magic number */
      printf("[[dnscat]] :: Sending a MSG packet (SEQ = 0x%04x, ACK = 0x%04x, %d bytes of data...\n", session->my_seq, session->their_seq, length);

      /* Create a packet with that data */
      packet = packet_create_msg(session->id, session->my_seq, session->their_seq, data, length);

      /* Send the packet */
      driver_send_packet(session->driver, packet);

      /* Free everything */
      packet_destroy(packet);
      safe_free(data);
      break;

    default:
      printf("[[ERROR]] :: Wound up in an unknown state: 0x%x\n", session->state);
      exit(1);
  }
}

void session_do_actions(session_t *session)
{
  /* Cleanup the incoming/outgoing buffers, if we can */
  clean_up_buffers(session);

  /* Receive if we can, then send if we can */
  do_recv_stuff(session);
  do_send_stuff(session);
}

