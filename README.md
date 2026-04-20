# cmpe148_minitcp

Mini-TCP-CC: Congestion-Controlled Reliable Transfer over UDP

This is for project 3, focused on UDP-based reliable file transfer protocol with TCP-inspired congestion control.

Files:
mini-tcp-cc.c --> The main entry point with both sender/receiver, using -s/-r flags to switch between the two
sender.c --> Sender logic: sliding window, congestion control, logging
reciever.c --> Receiver logic: in-order delivery, duplicate ACK generation

Flag Reference:
-S | Sender   | run in sender mode
-R | Reciever | run in reciever mode
-f | Both     | file to send or write to
-i | Sender   | IP address of the reciever
-p | Both     | UDP port number
-l | Sender   | simulate packet loss rate
-r | Sender   | simulate packet reorder rate 

Congestion Control Design:
All congestion control logic lives in the sender. The receiver's only role in CC 
is to send a duplicate ACK whenever an out-of-order packet arrives, which signals
a potential loss event to the sender. 



Protocal Summary:
Sender                          Receiver
  |                                 |
  |--- DATA (seq=0) --------------->|
  |--- DATA (seq=1) --------------->|
  |<----------- ACK (ack=0) --------|
  |<----------- ACK (ack=1) --------|  cwnd grows (slow start / cong. avoidance)
  |                                 |
  |--- DATA (seq=5, lost) ---X      |
  |<--- DUP ACK (ack=4) ------------|
  |<--- DUP ACK (ack=4) ------------|
  |<--- DUP ACK (ack=4) ------------|  3 dup ACKs → fast retransmit
  |--- DATA (seq=5, retransmit) --->|
  |                                 |
  |--- FIN ------------------------>|
  |                               (close)