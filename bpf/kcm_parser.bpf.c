/* KCM stream parser: recover message boundaries in the TCP receive path.
 *
 * strparser calls this once per candidate message with an skb whose data
 * starts at the message boundary, and interprets the return value as:
 *
 *   > 0  total length of a complete message
 *     0  need more bytes
 *   < 0  parse error, hand the socket back to userspace
 *
 * KDispatch's header carries a big-endian total length at offset 0, so the
 * whole parser is one load and a return.
 *
 * This file is the readable form of the program. server_kcm.cpp emits the
 * equivalent three instructions directly through bpf(2), so the project builds
 * and runs without clang or libbpf. The hand-written version uses LD_ABS, which
 * takes its skb pointer implicitly from r6 and so needs an "r6 = r1" prologue;
 * the C version below sidesteps that by calling a helper instead.
 *
 * To build this version instead:
 *
 *   clang -O2 -g -target bpf -c bpf/kcm_parser.bpf.c -o bpf/kcm_parser.bpf.o
 */

#include <linux/bpf.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

SEC("socket")
int kcm_parse(struct __sk_buff *skb)
{
	__u32 len;

	if (bpf_skb_load_bytes(skb, 0, &len, sizeof(len)) < 0)
		return 0;              /* not enough bytes for the header yet */

	return (int)bpf_ntohl(len);    /* total message length, header included */
}

char _license[] SEC("license") = "GPL";
