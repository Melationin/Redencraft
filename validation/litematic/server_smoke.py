"""Headless smoke test of the user's complete schematic, with no desktop client."""

import argparse
import collections
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'protocol775'))
from socket_test import handshake, packet, position, send, string, vint


def run(args):
    project = Path(__file__).resolve().parents[2]
    ids = {}
    for line in (project / 'assets/protocol775/packet_ids.tsv').read_text().splitlines():
        phase, flow, name, number = line.split('\t')
        ids[phase, flow, name.removeprefix('minecraft:')] = int(number)
    client = lambda name: ids['play', 'clientbound', name]
    server = lambda name: ids['play', 'serverbound', name]
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    process = subprocess.Popen(
        [str(args.server.resolve()), '--schematic', str(args.schematic.resolve()),
         '--port', '0', '--ticks', '100', '--view-distance', '2'],
        cwd=project, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding='utf-8', creationflags=flags)
    log, records, counts, messages = [], bytearray(), collections.Counter(), []
    imported = None
    try:
        while True:
            line = process.stdout.readline()
            if not line:
                raise AssertionError('Server failed before listening: ' + process.stderr.read())
            log.append(line.rstrip())
            if line.startswith('SCHEMATIC '):
                imported = dict(re.findall(r'(\w+)=(\S+)', line))
            match = re.search(r'LISTENING 127\.0\.0\.1:(\d+)', line)
            if match:
                port = int(match[1])
                break
        assert imported is not None
        assert int(imported['blocks']) == 70111
        assert int(imported['anvils_replaced']) == 28764
        assert int(imported['signs_skipped']) == 1
        assert int(imported['entities']) == 133
        assert int(imported['independent_entity_uuids']) == 133
        assert int(imported['forced_chunks']) == 2916
        with socket.create_connection(('127.0.0.1', port), timeout=20) as connection:
            handshake(connection, 2, port)
            send(connection, 0, string('SchematicProbe') + bytes(16))
            assert packet(connection)[0] == 2
            send(connection, 3)
            assert packet(connection) == (14, b'\0')
            send(connection, 7, b'\0')
            config_count = 0
            while packet(connection)[0] != 3:
                config_count += 1
            assert config_count == 30
            send(connection, 3)
            moved = False
            activated = False
            control_ack = False
            saw_far_chunk = False
            for _ in range(20000):
                ident, payload = packet(connection)
                counts[ident] += 1
                body = vint(ident) + payload
                records.extend(struct.pack('>I', len(body)) + body)
                if ident == client('disconnect'):
                    raise AssertionError('Disconnected: ' + repr(payload))
                if ident == client('keep_alive'):
                    send(connection, server('keep_alive'), payload)
                elif ident == client('chunk_batch_finished'):
                    send(connection, server('chunk_batch_received'), struct.pack('>f', 1000))
                elif ident == client('player_position'):
                    send(connection, server('accept_teleportation'), vint(1))
                elif ident == client('block_changed_ack') and args.activate_control:
                    control_ack |= payload == vint(1)
                elif ident == client('level_chunk_with_light'):
                    x, z = struct.unpack('>ii', payload[:8])
                    if moved and x == 49 and z == 0:
                        saw_far_chunk = True
                elif ident == client('system_chat'):
                    assert payload[0] == 8
                    length = struct.unpack('>H', payload[1:3])[0]
                    text = payload[3:3 + length].decode('utf-8')
                    assert len(payload) == length + 4 and payload[-1] == 0, 'Timing must be chat'
                    match = re.fullmatch(r'Tick (\d+): (\d+\.\d{3}) ms', text)
                    assert match, text
                    messages.append({'tick': int(match[1]), 'milliseconds': float(match[2])})
                    if len(messages) == 3 and args.activate_control and not activated:
                        send(connection, server('move_player_pos'),
                             struct.pack('>dddB', 2.5, 102, -1.5, 0))
                        send(connection, server('use_item_on'),
                             vint(0) + position(2, 101, 3) + vint(1) +
                             struct.pack('>fff??', .5, 1, .5, False, False) + vint(1))
                        activated = True
                    if len(messages) == (10 if args.activate_control else 3) and not moved:
                        send(connection, server('move_player_pos'),
                             struct.pack('>dddB', 792.5, 151, 8.5, 0))
                        moved = True
                    if len(messages) >= (13 if args.activate_control else 6) and saw_far_chunk:
                        break
            else:
                raise AssertionError('Schematic view/timing packets missing')
        assert counts[client('forget_level_chunk')] > 0
        assert not args.activate_control or control_ack
        assert all(a['tick'] < b['tick'] for a, b in zip(messages, messages[1:]))
        remaining, errors = process.communicate(timeout=30)
        log.extend(remaining.splitlines())
        assert process.returncode == 0, errors
        assert not errors.strip(), errors
        assert any(line.startswith('STOPPED ticks=100 ') for line in log)
        result = dict(
            source_sha256=hashlib.sha256(args.schematic.read_bytes()).hexdigest(),
            imported=imported, configuration_packets=config_count,
            play_packets=sum(counts.values()),
            chunks_received=counts[client('level_chunk_with_light')],
            chunks_forgotten=counts[client('forget_level_chunk')],
            far_corner_chunk_received=saw_far_chunk, tick_chat_samples=messages,
            control_right_click=args.activate_control,
            control_prediction_ack=control_ack,
            server_log=log, stderr=errors,
            scope='Native C++ headless TCP test; no Minecraft client, desktop or vanilla mechanism differential.')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        if args.packets:
            args.packets.write_bytes(records)
        print(json.dumps(result, indent=2))
    finally:
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--schematic', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--packets', type=Path)
    parser.add_argument('--activate-control', action='store_true')
    run(parser.parse_args())
