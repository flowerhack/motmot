#!/usr/bin/env python

import gevent
import gevent.socket
import gevent.server
import gevent.queue
import msgpack

import rpc


class Connection():
    new = gevent.queue.Queue()

    def __init__(self, socket, address):
        self.socket = socket
        self.address = address

        socket.setblocking(0)

        # Object queues
        self.__sendqueue = gevent.queue.Queue()
        self.__recvqueue = gevent.queue.Queue()

        # Spawn off reader-writer greenlets to process the queues
        gevent.spawn(self.__reader__)
        gevent.spawn(self.__writer__)

        Connection.new.put(self)

    def send(self, msg, **kwargs):
        self.__sendqueue.put(msg, **kwargs)
    def recv(self, **kwargs):
        return self.__recvqueue.get(**kwargs)

    def __reader__(self):
        unpacker = msgpack.Unpacker()
        fd = self.socket.fileno()
        while True:
            gevent.socket.wait_read(fd)
            unpacker.feed(self.socket.recv(4096))
            for val in unpacker:
                self.__recvqueue.put(val)

    def __writer__(self):
        packer = msgpack.Packer()
        fd = self.socket.fileno()
        for msg in self.__sendqueue:
            buf = packer.pack(msg)
            gevent.socket.wait_write(fd)
            self.socket.sendall(buf)

if __name__ == '__main__':
    gevent.spawn(rpc.new_connection_watcher, Connection.new)
    server = gevent.server.StreamServer(('127.0.0.1', 8888), Connection)
    server.serve_forever()
