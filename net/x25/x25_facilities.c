/*
 *	X.25 Packet Layer release 002
 *
 *	This is ALPHA test software. This code may break your machine,
 *	randomly fail to work with new releases, misbehave and/or generally
 *	screw up. It might even work.
 *
 *	This code REQUIRES 2.1.15 or higher
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	History
 *	X.25 001	Split from x25_subr.c
 *	mar/20/00	Daniela Squassoni Disabling/enabling of facilities
 *					  negotiation.
 *	apr/14/05	Shaun Pereira - Allow fast select with no restriction
 *					on response.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/x25.h>

/*
 * Parse a set of facilities into the facilities structures. Unrecognised
 *	facilities are written to the debug log file.
 */
int x25_parse_facilities(struct sk_buff *skb, struct x25_facilities *facilities,
		struct x25_dte_facilities *dte_facs, unsigned long *vc_fac_mask)
{
	unsigned char *p = skb->data;
	unsigned int len = *p++;

	*vc_fac_mask = 0;

	/*
	 * The kernel knows which facilities were set on an incoming call but
	 * currently this information is not available to userspace.  Here we
	 * give userspace who read incoming call facilities 0 length to indicate
	 * it wasn't set.
	 */
	dte_facs->calling_len = 0;
	dte_facs->called_len = 0;
	memset(dte_facs->called_ae, '\0', sizeof(dte_facs->called_ae));
	memset(dte_facs->calling_ae, '\0', sizeof(dte_facs->calling_ae));

	while (len > 0) {
		switch (*p & X25_FAC_CLASS_MASK) {
		case X25_FAC_CLASS_A:
			if (len < 2)
				return 0;
			switch (*p) {
			case X25_FAC_REVERSE:
				if((p[1] & 0x81) == 0x81) {
					facilities->reverse = p[1] & 0x81;
					*vc_fac_mask |= X25_MASK_REVERSE;
					break;
				}

				if((p[1] & 0x01) == 0x01) {
					facilities->reverse = p[1] & 0x01;
					*vc_fac_mask |= X25_MASK_REVERSE;
					break;
				}

				if((p[1] & 0x80) == 0x80) {
					facilities->reverse = p[1] & 0x80;
					*vc_fac_mask |= X25_MASK_REVERSE;
					break;
				}

				if(p[1] == 0x00) {
					facilities->reverse
						= X25_DEFAULT_REVERSE;
					*vc_fac_mask |= X25_MASK_REVERSE;
					break;
				}

			case X25_FAC_THROUGHPUT:
				facilities->throughput = p[1];
				*vc_fac_mask |= X25_MASK_THROUGHPUT;
				break;
			case X25_MARKER:
				break;
			default:
				printk(KERN_DEBUG "X.25: unknown facility "
				       "%02X, value %02X\n",
				       p[0], p[1]);
				break;
			}
			p   += 2;
			len -= 2;
			break;
		case X25_FAC_CLASS_B:
			if (len < 3)
				return 0;
			switch (*p) {
			case X25_FAC_PACKET_SIZE:
				facilities->pacsize_in  = p[1];
				facilities->pacsize_out = p[2];
				*vc_fac_mask |= X25_MASK_PACKET_SIZE;
				break;
			case X25_FAC_WINDOW_SIZE:
				facilities->winsize_in  = p[1];
				facilities->winsize_out = p[2];
				*vc_fac_mask |= X25_MASK_WINDOW_SIZE;
				break;
			default:
				printk(KERN_DEBUG "X.25: unknown facility "
				       "%02X, values %02X, %02X\n",
				       p[0], p[1], p[2]);
				break;
			}
			p   += 3;
			len -= 3;
			break;
		case X25_FAC_CLASS_C:
			if (len < 4)
				return 0;
			printk(KERN_DEBUG "X.25: unknown facility %02X, "
			       "values %02X, %02X, %02X\n",
			       p[0], p[1], p[2], p[3]);
			p   += 4;
			len -= 4;
			break;
		case X25_FAC_CLASS_D:
			if (len < p[1] + 2)
				return 0;
			switch (*p) {
			case X25_FAC_CALLING_AE:
				if (p[1] > X25_MAX_DTE_FACIL_LEN || p[1] <= 1)
					return 0;
				dte_facs->calling_len = p[2];
				memcpy(dte_facs->calling_ae, &p[3], p[1] - 1);
				*vc_fac_mask |= X25_MASK_CALLING_AE;
				break;
			case X25_FAC_CALLED_AE:
				if (p[1] > X25_MAX_DTE_FACIL_LEN || p[1] <= 1)
					return 0;
				dte_facs->called_len = p[2];
				memcpy(dte_facs->called_ae, &p[3], p[1] - 1);
				*vc_fac_mask |= X25_MASK_CALLED_AE;
				break;
			default:
				printk(KERN_DEBUG "X.25: unknown facility %02X,"
					"length %d\n", p[0], p[1]);
				break;
			}
			len -= p[1] + 2;
			p += p[1] + 2;
			break;
		}
	}

	return p - skb->data;
}

/*
 *	Create a set of facilities.
 */
int x25_create_facilities(unsigned char *buffer,
		struct x25_facilities *facilities,
		struct x25_dte_facilities *dte_facs, unsigned long facil_mask)
{
	unsigned char *p = buffer + 1;
	int len;

	if (!facil_mask) {
		/*
		 * Length of the facilities field in call_req or
		 * call_accept packets
		 */
		buffer[0] = 0;
		len = 1; /* 1 byte for the length field */
		return len;
	}

	if (facilities->reverse && (facil_mask & X25_MASK_REVERSE)) {
		*p++ = X25_FAC_REVERSE;
		*p++ = facilities->reverse;
	}

	if (facilities->throughput && (facil_mask & X25_MASK_THROUGHPUT)) {
		*p++ = X25_FAC_THROUGHPUT;
		*p++ = facilities->throughput;
	}

	if ((facilities->pacsize_in || facilities->pacsize_out) &&
	    (facil_mask & X25_MASK_PACKET_SIZE)) {
		*p++ = X25_FAC_PACKET_SIZE;
		*p++ = facilities->pacsize_in ? : facilities->pacsize_out;
		*p++ = facilities->pacsize_out ? : facilities->pacsize_in;
	}

	if ((facilities->winsize_in || facilities->winsize_out) &&
	    (facil_mask & X25_MASK_WINDOW_SIZE)) {
		*p++ = X25_FAC_WINDOW_SIZE;
		*p++ = facilities->winsize_in ? : facilities->winsize_out;
		*p++ = facilities->winsize_out ? : facilities->winsize_in;
	}

	if (facil_mask & (X25_MASK_CALLING_AE|X25_MASK_CALLED_AE)) {
		*p++ = X25_MARKER;
		*p++ = X25_DTE_SERVICES;
	}

	if (dte_facs->calling_len && (facil_mask & X25_MASK_CALLING_AE)) {
		unsigned bytecount = (dte_facs->calling_len % 2) ?
					dte_facs->calling_len / 2 + 1 :
					dte_facs->calling_len / 2;
		*p++ = X25_FAC_CALLING_AE;
		*p++ = 1 + bytecount;
		*p++ = dte_facs->calling_len;
		memcpy(p, dte_facs->calling_ae, bytecount);
		p += bytecount;
	}

	if (dte_facs->called_len && (facil_mask & X25_MASK_CALLED_AE)) {
		unsigned bytecount = (dte_facs->called_len % 2) ?
		dte_facs->called_len / 2 + 1 :
		dte_facs->called_len / 2;
		*p++ = X25_FAC_CALLED_AE;
		*p++ = 1 + bytecount;
		*p++ = dte_facs->called_len;
		memcpy(p, dte_facs->called_ae, bytecount);
		p+=bytecount;
	}

	len       = p - buffer;
	buffer[0] = len - 1;

	return len;
}

/*
 *	Try to reach a compromise on a set of facilities.
 *
 *	The only real problem is with reverse charging.
 */
int x25_negotiate_facilities(struct sk_buff *skb, struct sock *sk,
		struct x25_facilities *new, struct x25_dte_facilities *dte)
{
	struct x25_sock *x25 = x25_sk(sk);
	struct x25_facilities *ours = &x25->facilities;
	struct x25_facilities theirs;
	int len;

	memset(&theirs, 0, sizeof(theirs));
	memcpy(new, ours, sizeof(*new));

	len = x25_parse_facilities(skb, &theirs, dte, &x25->vc_facil_mask);

	/*
	 *	They want reverse charging, we won't accept it.
	 */
	if ((theirs.reverse & 0x01 ) && (ours->reverse & 0x01)) {
		SOCK_DEBUG(sk, "X.25: rejecting reverse charging request\n");
		return -1;
	}

	new->reverse = theirs.reverse;

	if (theirs.throughput) {
		if (theirs.throughput < ours->throughput) {
			SOCK_DEBUG(sk, "X.25: throughput negotiated down\n");
			new->throughput = theirs.throughput;
		}
	}

	if (theirs.pacsize_in && theirs.pacsize_out) {
		if (theirs.pacsize_in < ours->pacsize_in) {
			SOCK_DEBUG(sk, "X.25: packet size inwards negotiated down\n");
			new->pacsize_in = theirs.pacsize_in;
		}
		if (theirs.pacsize_out < ours->pacsize_out) {
			SOCK_DEBUG(sk, "X.25: packet size outwards negotiated down\n");
			new->pacsize_out = theirs.pacsize_out;
		}
	}

	if (theirs.winsize_in && theirs.winsize_out) {
		if (theirs.winsize_in < ours->winsize_in) {
			SOCK_DEBUG(sk, "X.25: window size inwards negotiated down\n");
			new->winsize_in = theirs.winsize_in;
		}
		if (theirs.winsize_out < ours->winsize_out) {
			SOCK_DEBUG(sk, "X.25: window size outwards negotiated down\n");
			new->winsize_out = theirs.winsize_out;
		}
	}

	return len;
}

/*
 *	Limit values of certain facilities according to the capability of the
 *      currently attached x25 link.
 */
void x25_limit_facilities(struct x25_facilities *facilities,
			  struct x25_neigh *nb)
{

	if (!nb->extended) {
		if (facilities->winsize_in  > 7) {
			printk(KERN_DEBUG "X.25: incoming winsize limited to 7\n");
			facilities->winsize_in = 7;
		}
		if (facilities->winsize_out > 7) {
			facilities->winsize_out = 7;
			printk( KERN_DEBUG "X.25: outgoing winsize limited to 7\n");
		}
	}
}
