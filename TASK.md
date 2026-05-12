Task: Implement a DPDK-Based Packet Forwarder with Flow Tracking

You are required to develop a DPDK-based application that captures packets from one network interface and forwards them to another, while tracking per-flow statistics.

Requirements:
	1.	Environment Setup:
	•	The application should be written in C.
	•	It should use DPDK for high-performance packet processing.
	•	The program should be able to run on a Linux system with DPDK installed.
	2.	Core Functionality:
	•	Initialize DPDK and allocate memory pools for packet buffers.
	•	Bind to two network interfaces (e.g., eth0 and eth1).
	•	Receive packets from one interface, process them, and forward them to another interface.
	3.	Packet Processing Logic:
	•	Parse incoming packets to extract basic Ethernet/IP header information.
	•	Drop packets if they are non-IP packets.
	•	Optionally modify the packet headers (e.g., change the destination MAC address).
	•	Forward valid packets to the appropriate interface.
	4.	Performance Considerations:
	•	Use multiple RX/TX queues to enable parallel processing by multiple worker threads.
	•	Optimize memory access patterns for cache efficiency.
	•	Implement basic batch processing for packets.
	5.	Flow Tracking & Affinity:
	•	Implement flow tracking using a 5-tuple (src IP, dst IP, src port, dst port, protocol).
	•	Ensure flow affinity is respected: all packets belonging to the same flow must be processed by the same worker thread/core.
	•	Use RSS (Receive Side Scaling) or a consistent hash function to distribute flows across queues while maintaining affinity.
	6.	Per-Flow Statistics Collection:
	•	For each tracked flow, collect the following metrics:
		-	Inbound bytes (rx_bytes)
		-	Outbound bytes (tx_bytes)
		-	Inbound packets (rx_packets)
		-	Outbound packets (tx_packets)
	•	Statistics should be updated in real-time as packets are processed.
	7.	Periodic Flow Statistics Export:
	•	Use one export file per core/thread (e.g., `flow_stats_core_<core_id>.csv`).
	•	At each export interval, append per-flow statistics to the corresponding core's file.
	•	Each export record should include: timestamp, flow 5-tuple (src IP, dst IP, src port, dst port, protocol), rx_bytes, tx_bytes, rx_packets, tx_packets.
	•	The same flow may have multiple records at different timestamps (one per export interval).
	•	Export interval should be configurable via command-line argument or configuration.
	8.	Flow Timeout & Cleanup:
	•	Flows that have been inactive (no packets received) for a configurable timeout period should be deleted from the tracking table.
	•	Timeout duration should be configurable via command-line argument or configuration.
	9.	Logging & Debugging:
	•	Implement logging for debugging purposes.

Expected Deliverables:
	•	A C program implementing the packet forwarder with flow tracking.
	•	A README.md with:
	•	Instructions on how to compile and run the program.
	•	List of dependencies.
	•	Description of the flow tracking mechanism and export format.
	•	Expected output and how to verify the program is working.
	•	Optional: A brief performance report measuring throughput with different packet sizes.

Bonus Challenges (Optional but Appreciated):
	1.	Use multiple cores for improved performance (e.g., separate RX and TX threads).
	2.	Support configurable flow table size with graceful handling when capacity is reached.

Evaluation Criteria:
	•	Code correctness & stability
	•	Understanding of DPDK concepts
	•	Correct implementation of flow affinity
	•	Accuracy of per-flow statistics
	•	Code efficiency & performance optimization
	•	Adherence to best practices in low-level networking
	•	Quality of documentation
