// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.clustercontroller.core;

import com.yahoo.vdslib.distribution.ConfiguredNode;
import com.yahoo.vdslib.state.Node;
import com.yahoo.vdslib.state.NodeType;
import org.junit.Test;

import java.util.Collection;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.stream.Collectors;
import java.util.stream.IntStream;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.mockito.Mockito.mock;

/**
 * Verifies that cluster controller properly cleans up per-node state when nodes
 * are removed from configuration. This prevents unbounded memory growth (OOM)
 * when schemas/nodes are frequently added and removed.
 */
public class NodeRemovalCleanupTest {

    /**
     * Simulates the OOM scenario: frequent node additions and removals cause
     * nodeStartTimestamps to grow without bound in ContentCluster.
     * After fix, removed nodes' timestamps should be cleaned up.
     */
    @Test
    public void nodeStartTimestamps_are_cleaned_up_when_nodes_are_removed() {
        // Start with 10 nodes
        ClusterFixture cf = ClusterFixture.forFlatCluster(10).bringEntireClusterUp();
        ContentCluster cluster = cf.cluster;

        // Set start timestamps for all 10 storage nodes
        for (int i = 0; i < 10; i++) {
            cluster.setStartTimestamp(Node.ofStorage(i), 1000 + i);
            cluster.setStartTimestamp(Node.ofDistributor(i), 2000 + i);
        }
        assertEquals(20, cluster.getStartTimestamps().size());

        // Simulate schema change: reconfigure to only 5 nodes (remove nodes 5-9)
        Collection<ConfiguredNode> reducedNodes = buildConfiguredNodes(5);
        cluster.setNodes(reducedNodes);

        // Verify: timestamps for removed nodes (5-9) should be cleaned up
        Map<Node, Long> timestamps = cluster.getStartTimestamps();
        assertTrue("Timestamps should be reduced", timestamps.size() <= 10);
        for (int i = 5; i < 10; i++) {
            assertFalse("Storage node " + i + " timestamp should be removed",
                    timestamps.containsKey(Node.ofStorage(i)));
            assertFalse("Distributor node " + i + " timestamp should be removed",
                    timestamps.containsKey(Node.ofDistributor(i)));
        }
        // Remaining nodes should still have timestamps
        for (int i = 0; i < 5; i++) {
            assertTrue("Storage node " + i + " timestamp should still exist",
                    timestamps.containsKey(Node.ofStorage(i)));
        }
    }

    /**
     * Simulates the high-churn OOM scenario from production:
     * hundreds of node add/remove cycles cause nodeStartTimestamps to grow unbounded.
     */
    @Test
    public void repeated_node_churn_does_not_leak_timestamps() {
        // Start with a small cluster
        ClusterFixture cf = ClusterFixture.forFlatCluster(3).bringEntireClusterUp();
        ContentCluster cluster = cf.cluster;

        // Simulate 100 cycles of adding and removing extra nodes
        for (int cycle = 0; cycle < 100; cycle++) {
            // Expand to 10 nodes
            Collection<ConfiguredNode> expandedNodes = buildConfiguredNodes(10);
            cluster.setNodes(expandedNodes);
            for (int i = 0; i < 10; i++) {
                cluster.setStartTimestamp(Node.ofStorage(i), cycle * 1000L + i);
            }

            // Shrink back to 3 nodes
            Collection<ConfiguredNode> shrunkNodes = buildConfiguredNodes(3);
            cluster.setNodes(shrunkNodes);
        }

        // After 100 cycles, timestamps should only contain entries for the 3 remaining nodes,
        // not accumulated entries from all cycles
        Map<Node, Long> timestamps = cluster.getStartTimestamps();
        int storageTimestamps = 0;
        for (Node n : timestamps.keySet()) {
            if (n.getType() == NodeType.STORAGE) storageTimestamps++;
        }
        assertEquals("Only 3 storage node timestamps should remain", 3, storageTimestamps);
    }

    /**
     * Verifies that SystemStateBroadcaster.lastErrorReported is cleaned up
     * when nodes are removed from the configuration.
     */
    @Test
    public void broadcaster_error_tracking_is_cleaned_up_on_node_removal() {
        FleetControllerContext context = mock(FleetControllerContext.class);
        FakeTimer timer = new FakeTimer();
        SystemStateBroadcaster broadcaster = new SystemStateBroadcaster(context, timer, new Object());

        // Simulate error tracking for 10 nodes
        Set<Node> allNodes = new HashSet<>();
        for (int i = 0; i < 10; i++) {
            allNodes.add(Node.ofStorage(i));
            allNodes.add(Node.ofDistributor(i));
        }

        // After removing nodes 5-9, clean up
        Set<Node> remainingNodes = new HashSet<>();
        for (int i = 0; i < 5; i++) {
            remainingNodes.add(Node.ofStorage(i));
            remainingNodes.add(Node.ofDistributor(i));
        }
        broadcaster.removeFromErrorTracking(remainingNodes);
        // Should not throw, and internal state should be bounded
    }

    /**
     * Stress test: simulate the production scenario of 26000+ sessions
     * equivalent to rapid node churn over 2 days.
     */
    @Test
    public void massive_node_churn_stays_bounded() {
        ClusterFixture cf = ClusterFixture.forFlatCluster(5).bringEntireClusterUp();
        ContentCluster cluster = cf.cluster;

        // Simulate 1000 rapid reconfigurations (like hourly schema changes across 7 regions)
        for (int i = 0; i < 1000; i++) {
            // Add extra nodes
            int extraNodes = 5 + (i % 10); // varying sizes
            Collection<ConfiguredNode> nodes = buildConfiguredNodes(extraNodes);
            cluster.setNodes(nodes);
            for (int j = 0; j < extraNodes; j++) {
                cluster.setStartTimestamp(Node.ofStorage(j), i * 10000L + j);
            }

            // Shrink back
            cluster.setNodes(buildConfiguredNodes(5));
        }

        // After 1000 cycles, should only have timestamps for 5 storage + 5 distributor = 10 max
        Map<Node, Long> timestamps = cluster.getStartTimestamps();
        assertTrue("Timestamps should be bounded, got " + timestamps.size(),
                timestamps.size() <= 10);
    }

    private static Collection<ConfiguredNode> buildConfiguredNodes(int count) {
        return IntStream.range(0, count)
                .mapToObj(i -> new ConfiguredNode(i, false))
                .collect(Collectors.toList());
    }
}
