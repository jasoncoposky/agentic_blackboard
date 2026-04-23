"use client";
import { useState, useEffect } from "react";

export default function Overview() {
  const [metrics, setMetrics] = useState({
    velocity: 142.5,
    toil: 0.12,
    latency: 18,
  });

  const [swarm, setSwarm] = useState([
    { id: "Apex_NC", status: "CONNECTED", type: "Hub" },
    { id: "Pittsburgh_PA", status: "CONNECTED", type: "Satellite" },
  ]);

  // Real-time metrics polling from ASOS Substrate (Port 8085)
  useEffect(() => {
    const fetchMetrics = async () => {
      try {
        const res = await fetch("http://localhost:8085/api/v1/query", {
          method: "POST",
          body: JSON.stringify({ match: "governance:swarm_health" }),
          headers: { "Content-Type": "application/json" }
        });
        const data = await res.json();
        if (data && data.length > 0) {
          // Parse the BSON-to-JSON fields from the substrate
          // Note: Substrate returns raw field strings; Monitor provides f64s
          setMetrics({
            velocity: parseFloat(data[0].knowledge_velocity) || 0,
            toil: parseFloat(data[0].toil_ratio) || 0,
            latency: parseInt(data[0].sync_latency_ms) || 0,
          });
        }
      } catch (e) {
        console.error("Failed to fetch swarm pulse:", e);
      }
    };

    const interval = setInterval(fetchMetrics, 2000);
    return () => clearInterval(interval);
  }, []);


  return (
    <div className="dashboard">
      <section className="metrics-grid">
        <div className="metric-card glass">
          <div className="metric-header">
            <span className="metric-label">Knowledge Velocity</span>
            <span className="trend positive">↑ 12%</span>
          </div>
          <div className="metric-value">{metrics.velocity} <span className="unit">Atoms/Min</span></div>
          <div className="progress-bar">
            <div className="progress-fill" style={{ width: '65%', background: 'var(--primary)' }}></div>
          </div>
        </div>

        <div className="metric-card glass">
          <div className="metric-header">
            <span className="metric-label">Toil Ratio</span>
            <span className="trend neutral">Steady</span>
          </div>
          <div className="metric-value">{metrics.toil} <span className="unit">Conflicts/Commit</span></div>
          <div className="progress-bar">
            <div className="progress-fill" style={{ width: '12%', background: 'var(--secondary)' }}></div>
          </div>
        </div>

        <div className="metric-card glass">
          <div className="metric-header">
            <span className="metric-label">Sync Latency</span>
            <span className="trend negative">↓ 4ms</span>
          </div>
          <div className="metric-value">{metrics.latency} <span className="unit">ms</span></div>
          <div className="progress-bar">
            <div className="progress-fill" style={{ width: '22%', background: '#ff00ff' }}></div>
          </div>
        </div>
      </section>

      <div className="secondary-grid">
        <section className="swarm-topology glass">
          <h3>Swarm Topology</h3>
          <div className="map-view">
            <div className="map-connecting-line"></div>
            {swarm.map(node => (
              <div key={node.id} className="node-item">
                <div className={`node-status-ring ${node.status.toLowerCase()}`}>
                  <div className="node-core"></div>
                </div>
                <div className="node-info">
                  <div className="node-id">{node.id}</div>
                  <div className="node-type">{node.type}</div>
                </div>
              </div>
            ))}
          </div>
          <div className="topology-stats">
            <div className="stat">Nodes Active: 2/2</div>
            <div className="stat">Consensus: Established</div>
          </div>
        </section>

        <section className="activity-ticker glass">
          <h3>Governor Activity</h3>
          <div className="ticker-list">
            <div className="ticker-item">
              <span className="timestamp">12:04:22</span>
              <span className="event">[Librarian] Synapse Created: <mark>atom-104</mark> → <mark>atom-221</mark></span>
            </div>
            <div className="ticker-item">
              <span className="timestamp">12:03:15</span>
              <span className="event">[Validator] Promoted <mark>atom-092</mark> to Universal Principle</span>
            </div>
            <div className="ticker-item alert">
              <span className="timestamp">11:58:04</span>
              <span className="event">[Monitor] High Sync Latency detected in Pittsburgh node</span>
            </div>
            <div className="ticker-item">
              <span className="timestamp">11:45:30</span>
              <span className="event">[Orchestrator] Distributed Heartbeat Re-synchronized (v0.4.2)</span>
            </div>
          </div>
        </section>
      </div>

      <style jsx>{`
        .dashboard {
          display: flex;
          flex-direction: column;
          gap: 24px;
        }

        .metrics-grid {
          display: grid;
          grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
          gap: 20px;
        }

        .metric-card {
          padding: 24px;
          display: flex;
          flex-direction: column;
          gap: 12px;
          transition: var(--transition);
        }

        .metric-card:hover {
          transform: translateY(-4px);
          box-shadow: 0 12px 24px hsla(230, 20%, 0%, 0.4);
          border-color: var(--primary);
        }

        .metric-header {
          display: flex;
          justify-content: space-between;
          align-items: center;
        }

        .metric-label {
          color: var(--text-muted);
          font-weight: 600;
          font-size: 0.9rem;
          text-transform: uppercase;
          letter-spacing: 0.05em;
        }

        .trend {
          font-size: 0.8rem;
          font-weight: 700;
          padding: 2px 8px;
          border-radius: 4px;
        }

        .trend.positive { color: #00ff88; background: rgba(0, 255, 136, 0.1); }
        .trend.neutral { color: var(--text-muted); background: rgba(255, 255, 255, 0.05); }
        .trend.negative { color: #ff00ff; background: rgba(255, 0, 255, 0.1); }

        .metric-value {
          font-size: 2.5rem;
          font-weight: 800;
          letter-spacing: -0.04em;
        }

        .unit {
          font-size: 1rem;
          color: var(--text-muted);
          font-weight: 500;
        }

        .progress-bar {
          height: 6px;
          background: hsla(230, 20%, 25%, 0.3);
          border-radius: 3px;
          overflow: hidden;
          margin-top: 8px;
        }

        .progress-fill {
          height: 100%;
          transition: width 1s ease-in-out;
          box-shadow: 0 0 10px inherit;
        }

        .secondary-grid {
          display: grid;
          grid-template-columns: 1fr 1fr;
          gap: 24px;
        }

        .swarm-topology {
          padding: 24px;
          display: flex;
          flex-direction: column;
        }

        .map-view {
          position: relative;
          display: flex;
          justify-content: space-around;
          align-items: center;
          padding: 60px 0;
          flex: 1;
        }

        .map-connecting-line {
          position: absolute;
          top: 50%;
          left: 20%;
          right: 20%;
          height: 2px;
          background: linear-gradient(to right, var(--primary), #00ff88);
          opacity: 0.3;
          z-index: 0;
        }

        .node-item {
          display: flex;
          flex-direction: column;
          align-items: center;
          gap: 12px;
          z-index: 1;
        }

        .node-status-ring {
          width: 48px;
          height: 48px;
          border-radius: 50%;
          display: flex;
          align-items: center;
          justify-content: center;
          padding: 4px;
          border: 2px solid transparent;
        }

        .node-status-ring.connected {
          border-color: var(--primary);
          box-shadow: 0 0 20px var(--primary-glow);
          animation: spin 8s linear infinite;
        }

        @keyframes spin {
          from { transform: rotate(0deg); }
          to { transform: rotate(360deg); }
        }

        .node-core {
          width: 100%;
          height: 100%;
          background: var(--text);
          border-radius: 50%;
        }

        .node-id {
          font-weight: 700;
          font-size: 0.9rem;
        }

        .node-type {
          font-size: 0.7rem;
          color: var(--text-muted);
          text-transform: uppercase;
        }

        .activity-ticker {
          padding: 24px;
          max-height: 400px;
          display: flex;
          flex-direction: column;
        }

        .ticker-list {
          display: flex;
          flex-direction: column;
          gap: 16px;
          overflow-y: auto;
          padding-right: 8px;
        }

        .ticker-item {
          font-size: 0.85rem;
          display: flex;
          gap: 12px;
          padding: 12px;
          border-radius: 8px;
          background: hsla(230, 20%, 50%, 0.03);
          border-left: 2px solid var(--surface-border);
        }

        .ticker-item.alert {
          border-left-color: var(--secondary);
          background: hsla(30, 100%, 50%, 0.05);
        }

        .timestamp {
          font-family: monospace;
          color: var(--text-muted);
        }

        .event mark {
          background: transparent;
          color: var(--primary);
          font-weight: 700;
        }

        .topology-stats {
          margin-top: auto;
          display: flex;
          justify-content: space-between;
          padding-top: 16px;
          border-top: 1px solid var(--surface-border);
          color: var(--text-muted);
          font-size: 0.75rem;
        }
      `}</style>
    </div>
  );
}
