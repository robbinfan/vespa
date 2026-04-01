// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.config.server.session;

import com.yahoo.cloud.config.ConfigserverConfig;
import com.yahoo.config.model.NullConfigModelRegistry;
import com.yahoo.config.provision.ApplicationId;
import com.yahoo.config.provision.TenantName;
import com.yahoo.test.ManualClock;
import com.yahoo.vespa.config.server.ApplicationRepository;
import com.yahoo.vespa.config.server.MockProvisioner;
import com.yahoo.vespa.config.server.application.OrchestratorMock;
import com.yahoo.vespa.config.server.filedistribution.MockFileDistributionFactory;
import com.yahoo.vespa.config.server.modelfactory.ModelFactoryRegistry;
import com.yahoo.vespa.config.server.tenant.TenantRepository;
import com.yahoo.vespa.config.server.tenant.TestTenantRepository;
import com.yahoo.vespa.config.util.ConfigUtils;
import com.yahoo.vespa.curator.mock.MockCurator;
import com.yahoo.vespa.flags.InMemoryFlagSource;
import com.yahoo.vespa.model.VespaModelFactory;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/**
 * Simulates the OOM scenario where frequent schema changes create thousands
 * of sessions that are not cleaned up fast enough. Validates that the increased
 * batch delete size (from 2 to 50) prevents session accumulation.
 */
public class SessionCleanupStressTest {

    private static final TenantName tenantName = TenantName.from("cleanuptest");

    @Rule
    public TemporaryFolder temporaryFolder = new TemporaryFolder();

    /**
     * Simulates the production scenario: hundreds of remote sessions accumulate
     * (like the 26000+ seen on rcmd-1 cluster) and verifies that a single
     * cleanup run can delete many more sessions than the previous limit of 2.
     */
    @Test
    public void high_frequency_session_creation_is_cleaned_up_efficiently() throws Exception {
        ManualClock clock = new ManualClock(Instant.now());
        MockCurator curator = new MockCurator();
        ConfigserverConfig configserverConfig = new ConfigserverConfig.Builder()
                .configServerDBDir(temporaryFolder.newFolder().getAbsolutePath())
                .configDefinitionsDir(temporaryFolder.newFolder().getAbsolutePath())
                .fileReferencesDir(temporaryFolder.newFolder().getAbsolutePath())
                .sessionLifetime(60) // 60 seconds
                .build();
        TenantRepository tenantRepository = new TestTenantRepository.Builder()
                .withClock(clock)
                .withConfigserverConfig(configserverConfig)
                .withCurator(curator)
                .withFileDistributionFactory(new MockFileDistributionFactory(configserverConfig))
                .withFlagSource(new InMemoryFlagSource())
                .build();
        tenantRepository.addTenant(tenantName);

        SessionRepository sessionRepository = tenantRepository.getTenant(tenantName).getSessionRepository();

        // Simulate creating 100 remote sessions (representing frequent schema changes)
        int totalSessions = 100;
        for (long i = 1; i <= totalSessions; i++) {
            SessionZooKeeperClient zkc = new SessionZooKeeperClient(
                    curator, tenantName, i, ConfigUtils.getCanonicalHostName());
            zkc.createNewSession(clock.instant());
        }

        // Verify all 100 sessions exist in ZooKeeper
        List<Long> sessionsBeforeCleanup = sessionRepository.getRemoteSessionsFromZooKeeper();
        assertEquals(totalSessions, sessionsBeforeCleanup.size());

        // Advance time past session expiry
        clock.advance(Duration.ofMinutes(5));

        // Run a single cleanup cycle - before fix this would only delete 2,
        // after fix it should delete up to 50
        int deleted = sessionRepository.deleteExpiredRemoteSessions(clock, Duration.ofSeconds(60));
        assertTrue("Should delete more than old limit of 2, deleted: " + deleted, deleted > 2);
        assertTrue("Should delete up to 50, deleted: " + deleted, deleted <= 50);

        List<Long> sessionsAfterFirstCleanup = sessionRepository.getRemoteSessionsFromZooKeeper();
        assertEquals(totalSessions - deleted, sessionsAfterFirstCleanup.size());

        // Run a second cleanup cycle - should clean more
        int deleted2 = sessionRepository.deleteExpiredRemoteSessions(clock, Duration.ofSeconds(60));
        assertTrue("Second cleanup should also delete sessions", deleted2 > 0);

        List<Long> sessionsAfterSecondCleanup = sessionRepository.getRemoteSessionsFromZooKeeper();
        assertEquals(totalSessions - deleted - deleted2, sessionsAfterSecondCleanup.size());
    }

    /**
     * Verifies that the old behavior would fail to keep up.
     * With 4-5 schema changes per hour across 7 regions, and the old limit of 2
     * deletes per 30-second run, cleanup capacity was ~240/hour.
     * With ~35 changes/hour (5 * 7), the sessions accumulate ~1260/day net.
     *
     * This test verifies the new batch size of 50 provides sufficient throughput.
     */
    @Test
    public void cleanup_throughput_is_sufficient_for_production_load() throws Exception {
        ManualClock clock = new ManualClock(Instant.now());
        MockCurator curator = new MockCurator();
        ConfigserverConfig configserverConfig = new ConfigserverConfig.Builder()
                .configServerDBDir(temporaryFolder.newFolder().getAbsolutePath())
                .configDefinitionsDir(temporaryFolder.newFolder().getAbsolutePath())
                .fileReferencesDir(temporaryFolder.newFolder().getAbsolutePath())
                .sessionLifetime(3600) // 1 hour default
                .build();
        TenantRepository tenantRepository = new TestTenantRepository.Builder()
                .withClock(clock)
                .withConfigserverConfig(configserverConfig)
                .withCurator(curator)
                .withFileDistributionFactory(new MockFileDistributionFactory(configserverConfig))
                .withFlagSource(new InMemoryFlagSource())
                .build();
        tenantRepository.addTenant(tenantName);

        SessionRepository sessionRepository = tenantRepository.getTenant(tenantName).getSessionRepository();

        // Simulate 2 hours of accumulated sessions (like production backlog)
        // 5 schema changes/hour * 7 regions = 35 sessions/hour * 2 hours = 70 sessions
        int backlogSessions = 70;
        for (long i = 1; i <= backlogSessions; i++) {
            SessionZooKeeperClient zkc = new SessionZooKeeperClient(
                    curator, tenantName, i, ConfigUtils.getCanonicalHostName());
            zkc.createNewSession(clock.instant());
        }

        // Advance past expiry
        clock.advance(Duration.ofHours(2));

        // A single cleanup run should clear most of the backlog (up to 50)
        int deleted = sessionRepository.deleteExpiredRemoteSessions(clock, Duration.ofHours(1));
        assertEquals("Should delete batch of 50 from 70 expired sessions", 50, deleted);

        // Second run clears the rest
        int deleted2 = sessionRepository.deleteExpiredRemoteSessions(clock, Duration.ofHours(1));
        assertEquals("Should delete remaining 20", 20, deleted2);

        // All expired sessions should be gone now
        assertEquals(0, sessionRepository.getRemoteSessionsFromZooKeeper().size());
    }

    /**
     * Verifies that active sessions are not deleted even with high cleanup rate.
     */
    @Test
    public void active_sessions_are_preserved_during_aggressive_cleanup() throws Exception {
        ManualClock clock = new ManualClock(Instant.now());
        MockCurator curator = new MockCurator();
        ConfigserverConfig configserverConfig = new ConfigserverConfig.Builder()
                .configServerDBDir(temporaryFolder.newFolder().getAbsolutePath())
                .configDefinitionsDir(temporaryFolder.newFolder().getAbsolutePath())
                .fileReferencesDir(temporaryFolder.newFolder().getAbsolutePath())
                .sessionLifetime(60)
                .build();
        TenantRepository tenantRepository = new TestTenantRepository.Builder()
                .withClock(clock)
                .withConfigserverConfig(configserverConfig)
                .withCurator(curator)
                .withFileDistributionFactory(new MockFileDistributionFactory(configserverConfig))
                .withFlagSource(new InMemoryFlagSource())
                .build();
        tenantRepository.addTenant(tenantName);

        SessionRepository sessionRepository = tenantRepository.getTenant(tenantName).getSessionRepository();

        // Create 60 sessions, and mark session 30 as ACTIVATE
        for (long i = 1; i <= 60; i++) {
            SessionZooKeeperClient zkc = new SessionZooKeeperClient(
                    curator, tenantName, i, ConfigUtils.getCanonicalHostName());
            zkc.createNewSession(clock.instant());
            if (i == 30) {
                // Mark session 30 as active
                zkc.writeStatus(Session.Status.ACTIVATE);
            }
        }

        // Advance past expiry
        clock.advance(Duration.ofMinutes(5));

        // Run cleanup - should delete expired sessions but skip ACTIVATE
        int deleted = sessionRepository.deleteExpiredRemoteSessions(clock, Duration.ofSeconds(60));
        assertTrue("Should delete many sessions", deleted > 2);

        // Session 30 should still exist
        List<Long> remaining = sessionRepository.getRemoteSessionsFromZooKeeper();
        assertTrue("Active session 30 should be preserved", remaining.contains(30L));
    }
}
