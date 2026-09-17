"use client";

export default function Governance() {
  const eus = [
    { id: "EU-2026-001", spec: "Distributed WAL XOR-Patching", status: "VALIDATED", impact: 0.15 },
    { id: "EU-2026-002", spec: "Librarian MinHash Optimization", status: "IN_PROGRESS", impact: 0.05 },
    { id: "EU-2026-003", spec: "SWEBOK KA-14 Matrix Alignment", status: "PROPOSED", impact: 0.02 },
  ];

  return (
    <div className="governance-container">
      <header className="gov-header">
        <h2 className="text-gradient">Daemon Governance Portal</h2>
        <p className="subtitle">Engineering Unit (EU) Lifecycle & Protocol Auditing</p>
      </header>

      <section className="eu-list">
        {eus.map(eu => (
          <div key={eu.id} className="eu-card glass">
            <div className="eu-header">
              <span className="eu-id">{eu.id}</span>
              <span className={`eu-status ${eu.status.toLowerCase()}`}>{eu.status}</span>
            </div>
            <h3 className="eu-spec">{eu.spec}</h3>
            <div className="eu-stats">
              <div className="stat">
                <span className="label">Error Budget Impact</span>
                <span className="value">-{eu.impact}%</span>
              </div>
              <div className="stat">
                <span className="label">Security Audit</span>
                <span className="value">PASSED</span>
              </div>
            </div>
            <div className="eu-progress">
              <div className="progress-track">
                <div className={`step ${eu.status !== 'PROPOSED' ? 'done' : ''}`}>Proposed</div>
                <div className={`step ${eu.status === 'VALIDATED' || eu.status === 'IN_PROGRESS' ? 'active' : ''}`}>In Progress</div>
                <div className={`step ${eu.status === 'VALIDATED' ? 'done' : ''}`}>Validated</div>
              </div>
            </div>
          </div>
        ))}
      </section>

      <style jsx>{`
        .governance-container {
          display: flex;
          flex-direction: column;
          gap: 32px;
        }

        .gov-header {
          display: flex;
          flex-direction: column;
          gap: 8px;
        }

        .subtitle {
          color: var(--text-muted);
          font-size: 1rem;
        }

        .eu-list {
          display: flex;
          flex-direction: column;
          gap: 20px;
        }

        .eu-card {
          padding: 24px;
          display: flex;
          flex-direction: column;
          gap: 16px;
        }

        .eu-header {
          display: flex;
          justify-content: space-between;
          align-items: center;
        }

        .eu-id {
          font-family: monospace;
          color: var(--primary);
          font-weight: 700;
        }

        .eu-status {
          font-size: 0.75rem;
          font-weight: 800;
          padding: 4px 12px;
          border-radius: 4px;
          background: rgba(255, 255, 255, 0.05);
        }

        .eu-status.validated { color: #00ff88; background: rgba(0, 255, 136, 0.1); }
        .eu-status.in_progress { color: var(--primary); background: rgba(0, 204, 255, 0.1); }

        .eu-spec {
          font-size: 1.4rem;
          margin: 0;
        }

        .eu-stats {
          display: flex;
          gap: 40px;
        }

        .stat {
          display: flex;
          flex-direction: column;
          gap: 4px;
        }

        .stat .label {
          font-size: 0.7rem;
          color: var(--text-muted);
          text-transform: uppercase;
          letter-spacing: 0.05em;
        }

        .stat .value {
          font-weight: 700;
          font-size: 0.9rem;
        }

        .eu-progress {
          margin-top: 12px;
          padding-top: 24px;
          border-top: 1px solid var(--surface-border);
        }

        .progress-track {
          display: flex;
          justify-content: space-between;
          position: relative;
        }

        .step {
          font-size: 0.8rem;
          font-weight: 600;
          color: var(--text-muted);
          position: relative;
          padding-bottom: 12px;
        }

        .step::after {
          content: "";
          position: absolute;
          bottom: 0;
          left: 50%;
          transform: translateX(-50%);
          width: 8px;
          height: 8px;
          border-radius: 50%;
          background: var(--surface-border);
        }

        .step.done { color: var(--text); }
        .step.done::after { background: #00ff88; box-shadow: 0 0 10px #00ff88; }
        
        .step.active { color: var(--primary); }
        .step.active::after { background: var(--primary); box-shadow: 0 0 10px var(--primary); }
      `}</style>
    </div>
  );
}
