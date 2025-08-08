## General Observations

- All comments, variable names, and the README are written in English.
- Each comment refers to the block of code immediately below it.
- I have tried to explain as clearly as possible what I implemented—I hope it worked!

## Three Way Handshake

- Implementing this synchronization mechanism was quite challenging, especially transmitting the last ACK.
- To reliably notify the client that the server received the ACK, I implemented a timer that waits to receive a SYN-ACK.
- If the server does **not** receive the last ACK, it resends the SYN-ACK because that ACK confirms the client received the SYN-ACK.
- Once the server receives the last ACK, the client will no longer receive the SYN-ACK again, and the connection is fully established.
- Issues arise if the network link is unreliable.
- I noticed that if a segment is lost, the synchronization breaks. To mitigate this, I send each handshake segment **ten times** to increase the probability that at least one reaches its destination.
- Previously, I used loops to resend the first SYN or SYN-ACK, but the final version sends each handshake segment ten times.
- After connection establishment, any duplicate or "garbage" ACKs or SYN-ACKs are ignored.
- This was the only way I managed to make it work; I believe there might be better approaches.
- The problem appeared when too many segments were in-flight, causing the client not to receive some SYN-ACKs.
- Consequently, the client would start sending data, but the server would think the client never received the SYN-ACK.
- In summary, I send each segment in the connection setup **ten times** to ensure reception.
- If all ten segments are lost, the resend process repeats.

## Data Sending

- I used a **ring buffer** to simulate the sliding window of the selective repeat technique.
- The handler communicates with the main thread through a **segment queue**.
- The segment queue stores payloads ready for the main thread to process, always in the correct order.
- The send and receive handlers manipulate the ring buffers.
- Both sender and receiver have their own segment queues and ring buffers.
- Process flow:
  - The sender calls `<send_data>` to enqueue data.
  - The sender handler continually checks the queue and fills the ring buffer with new data.
  - The handler also manages sending and acknowledging segments according to selective repeat.
  - When a buffer cell is full, the data is sent; the receiver emits an ACK upon reception.
  - On receiving the ACK, the sender marks the buffer cell as confirmed.
  - If no ACK is received within a timeout, the segment is resent.
  - This cycle continues, filling and emptying the buffer like a flowing cup.

- On the receiver side:
  - The receiver handler detects new segments and tries to save them in its ring buffer.
  - Duplicate segments cause duplicate ACKs to be sent to notify the sender.
  - The ring buffer must be emptied in order, starting from the head.
  - The handler moves received payloads into the segment queue and clears the ring buffer to receive more segments.
- This continues until the entire file is sent and received.
- The scheme is fast and reliable but introduces some lag for the sender.
- The sender might think the entire file has been sent, but it may only be cached in the handler.
- The queue has no strict space restrictions, so the whole file can be cached before any segment is sent.
- This may not be problematic but is worth noting.

## Conclusion

- Developing this homework made me appreciate how ingenious TCP really is.
- Our link conditions were fixed, so timers were set accordingly, but in real-life, dynamic schemes would be necessary.
- The project was challenging but fun, and I learned a lot about TCP.

---

**Note:** The implementation passes all tests on the checker successfully.
