#!/usr/bin/env python3
"""Headless managed-world TCP edit/save/restart proof; no desktop client."""
import re, socket, struct, subprocess, sys, time
from pathlib import Path

TARGET=(-13,0,-16)

def vint(n):
    n&=0xFFFFFFFF; out=bytearray()
    while True:
        b=n&127; n>>=7; out.append(b|(128 if n else 0))
        if not n:return bytes(out)
def string(s):
    b=s.encode(); return vint(len(b))+b
def send(s,i,p=b""):
    b=vint(i)+p; s.sendall(vint(len(b))+b)
def exact(s,n):
    o=bytearray()
    while len(o)<n:
        b=s.recv(n-len(o)); assert b,"unexpected EOF"; o.extend(b)
    return bytes(o)
def rv(s):
    n=0
    for k in range(5):
        b=exact(s,1)[0]; n|=(b&127)<<(7*k)
        if not b&128:return n
    raise AssertionError("bad VarInt")
def packet(s):
    n=rv(s); body=exact(s,n); ident=0
    for i,b in enumerate(body):
        ident|=(b&127)<<(7*i)
        if not b&128:return ident,body[i+1:]
    raise AssertionError("bad packet ID")
def pos(x,y,z):
    return struct.pack(">Q",((x&0x3FFFFFF)<<38)|((z&0x3FFFFFF)<<12)|(y&4095))
def mappings():
    ids={}
    for line in Path("/home/zhddsj/cpp/redstone-cpp/assets/protocol775/packet_ids.tsv").read_text().splitlines():
        fields=line.split("\t")
        if len(fields)>=4:
            ids[(fields[0],fields[1],fields[2].removeprefix("minecraft:"))]=int(fields[3])
    return ids
def wait_for_close(process,out):
    # Normal exit is part of the contract. Timeout diagnostics are preserved in
    # the log, but a killed child is always a failure.
    try:
        rest=process.communicate(timeout=20)[0]
    except subprocess.TimeoutExpired:
        process.kill()
        rest=process.communicate(timeout=5)[0]
        with out.open("a") as f:f.write(rest)
        raise AssertionError("managed server hung past TICKETED_WORLD_CLOSED")
    with out.open("a") as f:f.write(rest)
    assert process.returncode==0,("server exit",process.returncode)
    text=out.read_text()
    closes=re.findall(r"TICKETED_WORLD_CLOSED complete=(\d+) saved=(\d+) unsaved=(\d+)",text)
    assert closes,text
    # A disconnect may save before the explicit close, so saved=0 is valid.
    assert closes[-1][0]=="1" and closes[-1][1] in ("0","1") and closes[-1][2]=="0",closes[-1]
    assert "TICKETED_WORLD_FATAL" not in text,text
def read_until_play(s,ids):
    ident,_=packet(s)
    assert ident==ids[("play","clientbound","login")],("Play first packet",ident)
    player=False; chunk=False
    for _ in range(1000):
        ident,_=packet(s)
        if ident==ids[("play","clientbound","level_chunk_with_light")]:chunk=True
        if ident==ids[("play","clientbound","player_position")]:player=True
        if player and chunk:return
    raise AssertionError(("join incomplete",player,chunk))
def run_round(server,world,out,index):
    ids=mappings()
    process=subprocess.Popen(
        [str(server),"--no-console","--ticketed-world",str(world),
         "--port","0","--ticks","80","--spawn","-12.5,0.1,-12.5"],
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    try:
        lines=[]; port=None
        for _ in range(12):
            line=process.stdout.readline(); lines.append(line)
            match=re.search(r"LISTENING 127\.0\.0\.1:(\d+)",line)
            if match:port=int(match[1]); break
        out.write_text("".join(lines)); assert port is not None,lines
        with socket.create_connection(("127.0.0.1",port),timeout=8) as s:
            send(s,0,vint(775)+string("localhost")+struct.pack(">H",port)+vint(2))
            send(s,0,string("ManagedTcp")+bytes(16)); assert packet(s)[0]==2
            send(s,3); assert packet(s)==(14,b"\0")
            send(s,7,b"\0")
            configuration=0
            while True:
                ident,_=packet(s)
                if ident==ids[("configuration","clientbound","finish_configuration")]:break
                configuration+=1
            assert configuration==30,configuration
            send(s,3)
            read_until_play(s,ids)
            send(s,ids[("play","serverbound","accept_teleportation")],vint(1))
            send(s,ids[("play","serverbound","chunk_batch_received")],struct.pack(">f",9))
            sequence=1
            if index==1:
                send(s,ids[("play","serverbound","player_action")],
                     vint(0)+pos(*TARGET)+b"\1"+vint(sequence))
                expected_air=True
            else:
                send(s,ids[("play","serverbound","use_item_on")],
                     vint(0)+pos(*TARGET)+vint(1)+
                     struct.pack(">fff??",.5,1,.5,False,False)+vint(sequence))
                expected_air=False
            acknowledged=False; update=None
            for _ in range(500):
                ident,payload=packet(s)
                if ident==ids[("play","clientbound","block_update")] and payload[:8]==pos(*TARGET):
                    update=payload[8:]
                if ident==ids[("play","clientbound","block_changed_ack")]:
                    acknowledged=payload==vint(sequence)
                    break
            assert acknowledged,(index,"ack missing")
            assert update is not None,(index,"target update missing")
            assert (update==b"\0")==expected_air,(index,"unexpected target state",update)
        wait_for_close(process,out)
        return process.returncode
    finally:
        if process.poll() is None:process.kill()
        rest=process.communicate(timeout=8)[0]
        with out.open("a") as f:f.write(rest)

def main():
    server,world,out=Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3])
    run_round(server,world,out,1)
    run_round(server,world,out,2)
    print("MANAGED_TCP_EDIT_SAVE_RESTART_READBACK_OK")
if __name__=="__main__":main()
