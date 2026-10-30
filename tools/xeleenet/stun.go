// A STUN server (RFC 5389 Binding only), so the game can learn its public
// address from the same host as the lobbies instead of a third party's
// (net_punch.c's MELEE_XN_STUN). It answers every Binding request with
// XOR-MAPPED-ADDRESS and ignores everything else.

package main

import (
	"encoding/binary"
	"log"
	"net"
)

const stunMagic = 0x2112A442

func serveSTUN(addr string) error {
	conn, err := net.ListenUDP("udp", mustUDPAddr(addr))
	if err != nil {
		return err
	}
	log.Printf("stun on %s", conn.LocalAddr())
	go func() {
		buf := make([]byte, 1500)
		for {
			n, from, err := conn.ReadFromUDPAddrPort(buf)
			if err != nil {
				log.Printf("stun: %v", err)
				return
			}
			if reply := stunReply(buf[:n], from.Addr().Unmap().AsSlice(), from.Port()); reply != nil {
				conn.WriteToUDPAddrPort(reply, from)
			}
		}
	}()
	return nil
}

func mustUDPAddr(addr string) *net.UDPAddr {
	a, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		log.Fatalf("stun address %s: %v", addr, err)
	}
	return a
}

// stunReply answers a Binding request from ip:port; nil for anything else.
func stunReply(req []byte, ip []byte, port uint16) []byte {
	if len(req) < 20 || binary.BigEndian.Uint16(req[0:]) != 0x0001 ||
		binary.BigEndian.Uint32(req[4:]) != stunMagic || int(binary.BigEndian.Uint16(req[2:]))+20 != len(req) {
		return nil
	}
	family, alen := byte(1), 8
	if len(ip) == 16 {
		family, alen = 2, 20
	}
	out := make([]byte, 20+4+alen)
	binary.BigEndian.PutUint16(out[0:], 0x0101)
	binary.BigEndian.PutUint16(out[2:], uint16(4+alen))
	copy(out[4:20], req[4:20]) // the cookie and the transaction id
	a := out[20:]
	binary.BigEndian.PutUint16(a[0:], 0x0020)
	binary.BigEndian.PutUint16(a[2:], uint16(alen))
	a[5] = family
	binary.BigEndian.PutUint16(a[6:], port^(stunMagic>>16))
	key := out[4:20] // the cookie, then the transaction id (IPv6 only)
	for i := range ip {
		a[8+i] = ip[i] ^ key[i]
	}
	return out
}
