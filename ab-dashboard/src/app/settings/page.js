"use client";
import { useState, useEffect } from "react";

export default function SubstrateView() {
  const [nodes, setNodes] = useState([
    { id: "Apex_NC", role: "Apex/Consensus", uptime: "14d 2h", storage: "1.2GB", status: "HEALTHY" },
    { id: "Pittsburgh_PA", role: "Worker/Cache", uptime: "2d 5h", storage: "0.85GB", status: "HEALTHY" },
  ]);

  return (
    <div className="substrate-page">
      <header className="page-header">
        <div className="header-meta">
          <h1>Substrate Management</h1>
          <p className="text-muted">Distributed L3KVG Engine Status & Governance</p>
        </div>
        <div className="system-health glass">
          <div className="health-label">Global Integrity</div>
          <div className="health-value">99.98%</div>
        </div>
      </header>

      <div className="substrate-grid">
        <section className="cluster-topology glass">
          <div className="section-header">
            <h3>Swarm Nodes</h3>
            <button className="btn-action">Bootstrap Node</button>
          </div>
          <div className="node-list">
            {nodes.map(node => (
              <div key={node.id} className="node-card">
                <div className="node-icon-wrapper">
                  <div className="node-icon"></div>
                </div>
                <div className="node-details">
                  <div className="node-name">{node.id} <span className="badge">{node.status}</span></div>
                  <div className="node-role">{node.role}</div>
                  <div className="node-stats">
                    <span>Uptime: {node.uptime}</span>
                    <span className="dot"></span>
                    <span>Storage: {node.storage}</span>
                  </div>
                </div>
              </div>
            ))}
          </div>
        </section>

        <section className="shard-distribution glass">
          <h3>Knowledge Sharding</h3>
          <div className="shard-grid">
            {Array.from({ length: 15 }).map((_, i) => (
              <div key={i} className={`shard-cell ${i < 8 ? 'active' : ''}`} title={`KA ${i+1}`}>
                {i+1}
              </div>
            ))}
          </div>
          <p className="hint">Shards 1-8 currently replicated across 2 nodes.</p>
        </section>

        <section className="persistence-metrics glass">
          <h3>Persistence (IOPS)</h3>
          <div className="bar-chart">
            <div className="bar" style={{height: '60%'}}></div>
            <div className="bar" style={{height: '85%'}}></div>
            <div className="bar" style={{height: '40%'}}></div>
            <div className="bar" style={{height: '70%'}}></div>
            <div className="bar" style={{height: '55%'}}></div>
          </div>
          <div className="chart-legend">
            <span>Read</span>
            <span>Write</span>
            <span>WAL Flush</span>
          </div>
        </section>

        <section className="config-tuning glass">
          <h3>Governance Tuning</h3>
          <div className="config-item">
            <span>Heartbeat Interval</span>
            <input type="range" defaultValue="500" />
            <span className="val">500ms</span>
          </div>
          <div className="config-item">
            <span>Consensus Threshold</span>
            <input type="range" defaultValue="66" />
            <span className="val">2/3 Quorum</span>
          </div>
          <div className="config-item">
            <span>Discovery Port</span>
            <span className="val">8085</span>
          </div>
        </section>
      </div>

      <style jsx>{`
        .substrate-page {
          display: flex;
          flex-direction: column;
          gap: 24px;
        }

        .page-header {
          display: flex;
          justify-content: space-between;
          align-items: center;
        }

        .system-health {
          padding: 16px 24px;
          text-align: right;
        }

        .health-label {
          font-size: 0.75rem;
          color: var(--text-muted);
          text-transform: uppercase;
          font-weight: 700;
        }

        .health-value {
          font-size: 1.5rem;
          color: var(--primary);
          font-weight: 800;
        }

        .substrate-grid {
          display: grid;
          grid-template-columns: 3fr 2fr;
          grid-template-rows: auto auto;
          gap: 24px;
        }

        .section-header {
          display: flex;
          justify-content: space-between;
          align-items: center;
          margin-bottom: 24px;
        }

        .btn-action {
          background: var(--primary);
          color: #000;
          border: none;
          padding: 8px 16px;
          border-radius: 8px;
          font-weight: 700;
          font-size: 0.8rem;
          cursor: pointer;
        }

        .cluster-topology {
          padding: 24px;
          grid-row: span 2;
        }

        .node-list {
          display: flex;
          flex-direction: column;
          gap: 16px;
        }

        .node-card {
          display: flex;
          gap: 16px;
          padding: 16px;
          background: hsla(230, 20%, 5%, 0.3);
          border-radius: 12px;
          border: 1px solid var(--surface-border);
        }

        .node-icon-wrapper {
          width: 40px;
          height: 40px;
          background: hsla(180, 100%, 50%, 0.1);
          border-radius: 8px;
          display: flex;
          align-items: center;
          justify-content: center;
        }

        .node-icon {
          width: 16px;
          height: 16px;
          background: var(--primary);
          border-radius: 2px;
          box-shadow: 0 0 10px var(--primary-glow);
        }

        .node-name {
          font-weight: 700;
          font-size: 1rem;
          display: flex;
          align-items: center;
          gap: 12px;
        }

        .badge {
          font-size: 0.65rem;
          padding: 2px 6px;
          background: rgba(0, 255, 136, 0.1);
          color: #00ff88;
          border-radius: 4px;
          text-transform: uppercase;
        }

        .node-role {
          font-size: 0.8rem;
          color: var(--text-muted);
          margin-bottom: 8px;
        }

        .node-stats {
          font-size: 0.75rem;
          color: var(--text-muted);
          display: flex;
          align-items: center;
          gap: 8px;
        }

        .dot {
          width: 3px;
          height: 3px;
          background: var(--text-muted);
          border-radius: 50%;
        }

        .shard-distribution {
          padding: 24px;
        }

        .shard-grid {
          display: grid;
          grid-template-columns: repeat(5, 1fr);
          gap: 8px;
          margin: 16px 0;
        }

        .shard-cell {
          height: 32px;
          background: hsla(230, 20%, 15%, 0.5);
          border-radius: 4px;
          display: flex;
          align-items: center;
          justify-content: center;
          font-size: 0.75rem;
          font-weight: 700;
          color: var(--text-muted);
        }

        .shard-cell.active {
          background: hsla(180, 100%, 50%, 0.15);
          color: var(--primary);
          border: 1px solid var(--primary);
        }

        .hint {
          font-size: 0.7rem;
          color: var(--text-muted);
          font-style: italic;
        }

        .persistence-metrics {
          padding: 24px;
        }

        .bar-chart {
          display: flex;
          align-items: flex-end;
          gap: 12px;
          height: 100px;
          margin: 20px 0;
          padding: 0 10px;
          border-bottom: 1px solid var(--surface-border);
        }

        .bar {
          flex: 1;
          background: linear-gradient(to top, var(--primary), #00ff88);
          border-radius: 4px 4px 0 0;
          opacity: 0.7;
        }

        .chart-legend {
          display: flex;
          justify-content: space-between;
          font-size: 0.65rem;
          color: var(--text-muted);
        }

        .config-tuning {
          padding: 24px;
        }

        .config-item {
          display: flex;
          flex-direction: column;
          gap: 8px;
          margin-bottom: 20px;
          font-size: 0.85rem;
        }

        .config-item input {
          width: 100%;
          accent-color: var(--primary);
        }

        .val {
          color: var(--primary);
          font-weight: 700;
          font-family: monospace;
          text-align: right;
        }
      `}</style>
    </div>
  );
}
