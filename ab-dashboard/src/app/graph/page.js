"use client";
import { useState, useEffect } from "react";
import GraphMap from "../../components/GraphMap";

export default function KnowledgeStudio() {
  const [filter, setFilter] = useState("ALL");
  const [viewMode, setViewMode] = useState("MAP"); // Default to Map View for "Wow" factor
  const [graphData, setGraphData] = useState({ nodes: [], edges: [] });
  const [isLoading, setIsLoading] = useState(true);
  const [selectedNode, setSelectedNode] = useState(null);


  // Fetch real-time graph snapshot from Agentic Blackboard Substrate
  useEffect(() => {
    const fetchSnapshot = async () => {
      try {
        const res = await fetch("http://localhost:8085/api/v1/graph/snapshot");
        const data = await res.json();
        setGraphData(data);
        setIsLoading(false);
      } catch (e) {
        console.error("Failed to fetch graph topology:", e);
      }
    };

    fetchSnapshot();
    const interval = setInterval(fetchSnapshot, 10000); // 10s refresh for graph
    return () => clearInterval(interval);
  }, []);

  const categories = ["ALL", "DESIGN", "CONFIG_MGMT", "SRE", "TESTING", "ORPHAN"];

  const handlePromote = async (uuid) => {
    try {
      const res = await fetch("http://localhost:8085/api/v1/cpb/promote", {
        method: "POST",
        body: JSON.stringify({ uuid })
      });
      if (res.ok) {
        // Refresh graph to show updated status
        const res2 = await fetch("http://localhost:8085/api/v1/graph/snapshot");
        setGraphData(await res2.json());
      }
    } catch (e) {
      console.error("Promotion failed:", e);
    }
  };

  const handleMaterialize = async (atomId) => {
    try {
      await fetch("http://localhost:8085/api/v1/nucleus/materialize", {
        method: "POST",
        body: JSON.stringify({ 
          atom_id: atomId, 
          behavior: "status_ring",
          label: "Agentic Blackboard Materialization"
        })
      });
      alert("Command dispatched to Nucleus Governor.");
    } catch (e) {
      console.error("Materialization failed:", e);
    }
  };

  return (
    <div className="studio-container">
      <header className="studio-header">
        <div className="header-top">
          <h2 className="text-gradient">Librarian Knowledge Studio</h2>
          <div className="view-toggle glass">
             <button 
               className={viewMode === "STUDIO" ? "active" : ""} 
               onClick={() => setViewMode("STUDIO")}
             >
               Studio View
             </button>
             <button 
               className={viewMode === "MAP" ? "active" : ""} 
               onClick={() => setViewMode("MAP")}
             >
               Map View
             </button>
          </div>
        </div>
        
        {viewMode === "STUDIO" && (
          <div className="filter-chips">
            {categories.map(cat => (
              <button 
                key={cat} 
                className={`chip ${filter === cat ? 'active' : ''}`}
                onClick={() => setFilter(cat)}
              >
                {cat}
              </button>
            ))}
          </div>
        )}
      </header>

      <div className="main-stage">
        {isLoading && (
          <div className="loading-overlay glass">
            <div className="spinner"></div>
            <span>Synchronizing Substrate Topology...</span>
          </div>
        )}

        {viewMode === "MAP" ? (
          <GraphMap 
            data={graphData} 
            selectedNodeId={selectedNode?.id} 
            onNodeSelect={setSelectedNode} 
          />
        ) : (
          <div className="atom-grid">
            {graphData.nodes
              .filter(n => n.type === "ATOM") // Studio View is for Knowledge Atoms
              .filter(a => filter === "ALL" || a.ka === filter)
              .map(atom => (
                <div key={atom.id} className={`atom-card glass`}>
                  <div className="atom-meta">
                    <span className="atom-ka">{atom.ka}</span>
                    <span className={`atom-status ${(atom.status || "").toLowerCase()}`}>{atom.status}</span>
                  </div>
                  
                  <div className="atom-anchors">
                    <div className="anchor">
                      <span className="label">Project</span>
                      <span className={`value ${atom.project === 'MISSING' ? 'missing' : ''}`}>{atom.project || "N/A"}</span>
                    </div>
                    <div className="anchor">
                      <span className="label">Author</span>
                      <span className={`value ${atom.author === 'MISSING' ? 'missing' : ''}`}>{atom.author || "N/A"}</span>
                    </div>
                  </div>

                  <p className="atom-statement">{atom.statement}</p>
                  
                  <div className="atom-footer">
                    <span className="atom-id">{atom.id}</span>
                    <div className="atom-actions">
                      {atom.status === "VALIDATED" && (
                        <button 
                          className="btn-promote"
                          onClick={() => handlePromote(atom.id)}
                        >
                          Promote
                        </button>
                      )}
                      <button 
                        className="btn-view"
                        onClick={() => setSelectedNode(atom)}
                      >
                        Details
                      </button>
                    </div>
                  </div>
                </div>
              ))}
          </div>
        )}
      </div>

      <div className={`sidebar glass ${selectedNode ? 'open' : ''}`}>
        {selectedNode && (
          <div className="sidebar-content">
            <div className="sidebar-header">
              <h3>Node Details</h3>
              <button className="close-btn" onClick={() => setSelectedNode(null)}>✕</button>
            </div>
            
            <div className="sidebar-meta">
              <span className={`badge ${selectedNode.type.toLowerCase()}`}>{selectedNode.type}</span>
              {selectedNode.ka !== undefined && <span className="badge ka">KA: {selectedNode.ka}</span>}
              {selectedNode.status && <span className={`badge status ${(selectedNode.status).toLowerCase()}`}>{selectedNode.status}</span>}
              {selectedNode.tags && selectedNode.tags.map(tag => (
                <span key={tag} className="badge tag">{tag}</span>
              ))}
            </div>

            <div className="sidebar-actions">
              <button 
                className="btn-primary"
                onClick={() => handleMaterialize(selectedNode.id)}
              >
                Materialize in Nucleus
              </button>
            </div>

            <div className="sidebar-id">
              <label>ID</label>
              <span>{selectedNode.id}</span>
            </div>

            {selectedNode.statement && (
              <div className="sidebar-section">
                <label>Statement</label>
                <p>{selectedNode.statement}</p>
              </div>
            )}

            {selectedNode.project && (
              <div className="sidebar-section">
                <label>Project</label>
                <span>{selectedNode.project}</span>
              </div>
            )}

            {selectedNode.author && (
              <div className="sidebar-section">
                <label>Author</label>
                <span>{selectedNode.author}</span>
              </div>
            )}

            {selectedNode.content && (
              <div className="sidebar-section">
                <label>Content</label>
                <div className="content-box">
                  <pre>{selectedNode.content}</pre>
                </div>
              </div>
            )}

            {selectedNode.wellness && (
              <div className="sidebar-section wellness-section">
                <label>Wellness & Activity</label>
                <div className="wellness-grid">
                  <div className="wellness-metric">
                    <span className="metric-val">{selectedNode.wellness.mood_sentiment > 0 ? "+" : ""}{selectedNode.wellness.mood_sentiment}</span>
                    <span className="metric-lbl">Mood / Sentiment</span>
                  </div>
                  <div className="wellness-metric">
                    <span className="metric-val">{selectedNode.wellness.energy_level}/10</span>
                    <span className="metric-lbl">Energy Level</span>
                  </div>
                  <div className="wellness-metric">
                    <span className="metric-val">{selectedNode.wellness.sleep_hours}h</span>
                    <span className="metric-lbl">Sleep Duration</span>
                  </div>
                  {selectedNode.wellness.step_count > 0 && (
                    <div className="wellness-metric">
                      <span className="metric-val">{selectedNode.wellness.step_count.toLocaleString()}</span>
                      <span className="metric-lbl">Steps</span>
                    </div>
                  )}
                  {selectedNode.wellness.active_minutes > 0 && (
                    <div className="wellness-metric">
                      <span className="metric-val">{selectedNode.wellness.active_minutes}m</span>
                      <span className="metric-lbl">Active Minutes</span>
                    </div>
                  )}
                  {selectedNode.wellness.activity_type && (
                    <div className="wellness-metric full-width">
                      <span className="metric-val activity">{selectedNode.wellness.activity_type}</span>
                      <span className="metric-lbl">Current Activity</span>
                    </div>
                  )}
                </div>
              </div>
            )}

            {selectedNode.education && (
              <div className="sidebar-section education-section">
                <label>Education & Learning</label>
                <div className="education-grid">
                  <div className="education-metric">
                    <span className="metric-val">{selectedNode.education.progress_percent}%</span>
                    <span className="metric-lbl">Progress</span>
                  </div>
                  <div className="education-metric">
                    <span className="metric-val">{selectedNode.education.focus_duration_minutes}m</span>
                    <span className="metric-lbl">Study Time</span>
                  </div>
                  {selectedNode.education.institution_platform && (
                    <div className="education-metric full-width">
                      <span className="metric-val platform">{selectedNode.education.institution_platform}</span>
                      <span className="metric-lbl">Institution / Platform</span>
                    </div>
                  )}
                  {selectedNode.education.resource_type && (
                    <div className="education-metric">
                      <span className="metric-val type">{selectedNode.education.resource_type}</span>
                      <span className="metric-lbl">Medium / Resource</span>
                    </div>
                  )}
                  {selectedNode.education.credential_uuid && (
                    <div className="education-metric">
                      <span className="metric-val credential">{selectedNode.education.credential_uuid}</span>
                      <span className="metric-lbl">Credential Ref</span>
                    </div>
                  )}
                </div>
              </div>
            )}

            <div className="sidebar-section">
              <label>Raw Data</label>
              <pre className="json-dump">{JSON.stringify(selectedNode, null, 2)}</pre>
            </div>
          </div>
        )}
      </div>

      <style jsx>{`
        .studio-container {


          display: flex;
          flex-direction: column;
          gap: 32px;
        }

        .studio-header {
          display: flex;
          flex-direction: column;
          gap: 20px;
        }

        .main-stage {
          position: relative;
          min-height: 600px;
        }


        .header-top {
          display: flex;
          justify-content: space-between;
          align-items: center;
        }

        .view-toggle {
          display: flex;
          padding: 4px;
          border-radius: 12px;
          gap: 4px;
        }

        .view-toggle button {
          border: none;
          background: transparent;
          color: var(--text-muted);
          padding: 8px 16px;
          border-radius: 8px;
          font-weight: 700;
          cursor: pointer;
          transition: var(--transition);
        }

        .view-toggle button.active {
          background: var(--primary);
          color: var(--background);
        }

        .chip {
          padding: 8px 16px;
          border-radius: 20px;
          background: hsla(230, 20%, 20%, 0.5);
          border: 1px solid var(--surface-border);
          color: var(--text-muted);
          font-size: 0.75rem;
          font-weight: 600;
          cursor: pointer;
          transition: var(--transition);
        }


        .chip:hover {
          background: hsla(230, 20%, 30%, 0.5);
          color: var(--text);
        }

        .chip.active {
          background: var(--primary);
          color: var(--background);
          border-color: var(--primary);
        }

        .atom-grid {
          display: grid;
          grid-template-columns: repeat(auto-fill, minmax(360px, 1fr));
          gap: 24px;
        }

        .atom-card {
          padding: 24px;
          display: flex;
          flex-direction: column;
          gap: 16px;
          transition: transform 0.3s var(--easing), box-shadow 0.3s var(--easing);
        }

        .atom-card:hover {
          transform: translateY(-4px);
          box-shadow: 0 12px 24px rgba(0,0,0,0.4);
          background: hsla(230, 20%, 15%, 0.6);
        }

        .principle-border { border: 1px solid var(--secondary); box-shadow: 0 0 15px var(--secondary-glow); }
        .orphan-border { border: 1px solid #ff0055; box-shadow: 0 0 15px rgba(255, 0, 85, 0.2); }

        .atom-meta {
          display: flex;
          justify-content: space-between;
          align-items: center;
        }

        .atom-ka {
          font-size: 0.7rem;
          font-weight: 800;
          color: var(--primary);
          background: hsla(180, 100%, 50%, 0.1);
          padding: 4px 8px;
          border-radius: 4px;
        }

        .atom-status {
          font-size: 0.7rem;
          font-weight: 700;
          text-transform: uppercase;
        }

        .atom-status.principle { color: var(--secondary); }
        .atom-status.validated { color: #00ff88; }
        .atom-status.proposed { color: var(--text-muted); }
        .atom-status.orphan { color: #ff0055; }

        .atom-anchors {
          display: flex;
          gap: 24px;
          padding: 12px;
          background: hsla(230, 20%, 5%, 0.3);
          border-radius: 8px;
          border: 1px solid var(--surface-border);
        }

        .anchor {
          display: flex;
          flex-direction: column;
          gap: 4px;
        }

        .anchor .label {
          font-size: 0.6rem;
          text-transform: uppercase;
          color: var(--text-muted);
          letter-spacing: 0.05em;
        }

        .anchor .value {
          font-size: 0.8rem;
          font-weight: 700;
          color: var(--text);
        }

        .anchor .value.missing {
          color: #ff0055;
          text-decoration: underline dotted;
        }

        .atom-statement {
          font-size: 1.1rem;
          font-weight: 500;
          line-height: 1.4;
          flex: 1;
        }

        .atom-footer {
          display: flex;
          justify-content: space-between;
          align-items: center;
          margin-top: 12px;
          padding-top: 16px;
          border-top: 1px solid var(--surface-border);
        }

        .atom-id {
          font-family: monospace;
          font-size: 0.8rem;
          color: var(--text-muted);
        }

        button {
          padding: 6px 12px;
          border-radius: 6px;
          font-size: 0.75rem;
          font-weight: 600;
          cursor: pointer;
          background: transparent;
          border: 1px solid var(--surface-border);
          color: var(--text);
          transition: var(--transition);
        }

        button:hover { border-color: var(--text); }
        .btn-promote { background: var(--secondary); border-color: var(--secondary); color: var(--background); }
        .loading-overlay {
          position: absolute;
          top: 0;
          left: 0;
          right: 0;
          bottom: 0;
          z-index: 100;
          display: flex;
          flex-direction: column;
          align-items: center;
          justify-content: center;
          gap: 20px;
          background: hsla(230, 20%, 5%, 0.4); /* Reduced opacity */
          backdrop-filter: blur(4px); /* Reduced blur */
          color: var(--text-muted);

          font-weight: 700;
          letter-spacing: 0.1em;
          text-transform: uppercase;
          font-size: 0.8rem;
        }

        .spinner {
          width: 40px;
          height: 40px;
          border: 3px solid var(--surface-border);
          border-top-color: var(--primary);
          border-radius: 50%;
          animation: spin 1s linear infinite;
        }

        @keyframes spin {
          to { transform: rotate(360deg); }
        }

        .sidebar {
          position: fixed;
          top: 0;
          right: -400px;
          width: 400px;
          height: 100vh;
          background: hsla(230, 20%, 8%, 0.95);
          backdrop-filter: blur(20px);
          border-left: 1px solid var(--surface-border);
          z-index: 1000;
          transition: right 0.3s cubic-bezier(0.16, 1, 0.3, 1);
          box-shadow: -5px 0 30px rgba(0,0,0,0.5);
          overflow-y: auto;
        }

        .sidebar.open {
          right: 0;
        }

        .sidebar-content {
          padding: 24px;
          display: flex;
          flex-direction: column;
          gap: 24px;
        }

        .sidebar-actions {
          padding: 16px 0;
          border-bottom: 1px solid var(--surface-border);
        }

        .btn-primary {
          width: 100%;
          padding: 12px;
          background: var(--primary);
          color: var(--background);
          border: none;
          border-radius: 8px;
          font-weight: 700;
          text-transform: uppercase;
          letter-spacing: 0.1em;
          box-shadow: 0 4px 15px var(--primary-glow);
        }

        .btn-primary:hover {
          transform: scale(1.02);
          filter: brightness(1.1);
        }

        .sidebar-header {
          display: flex;
          justify-content: space-between;
          align-items: center;
          border-bottom: 1px solid var(--surface-border);
          padding-bottom: 16px;
        }

        .sidebar-header h3 {
          margin: 0;
          color: var(--primary);
        }

        .close-btn {
          background: none;
          border: none;
          color: var(--text-muted);
          font-size: 1.2rem;
          cursor: pointer;
          padding: 4px;
        }

        .close-btn:hover {
          color: var(--text);
        }

        .sidebar-meta {
          display: flex;
          flex-wrap: wrap;
          gap: 8px;
        }

        .badge {
          padding: 4px 8px;
          border-radius: 4px;
          font-size: 0.7rem;
          font-weight: 700;
          text-transform: uppercase;
        }
        .badge.ka { background: hsla(230, 20%, 40%, 0.3); border-color: hsla(230, 20%, 60%, 0.5); }
        .badge.status.validated { background: hsla(150, 60%, 40%, 0.3); border-color: hsla(150, 60%, 60%, 0.5); }
        .badge.status.uncertain { background: hsla(350, 60%, 40%, 0.3); border-color: hsla(350, 60%, 60%, 0.5); }
        .badge.tag { 
          background: linear-gradient(135deg, hsla(200, 80%, 40%, 0.3) 0%, hsla(200, 80%, 20%, 0.4) 100%); 
          border: 1px solid hsla(200, 80%, 60%, 0.4); 
          color: hsla(200, 80%, 85%, 1);
          text-transform: uppercase;
          font-weight: 800;
          letter-spacing: 0.08em;
          box-shadow: 0 2px 4px rgba(0,0,0,0.2);
          transition: all 0.2s ease;
          cursor: default;
        }
        .badge.tag:hover {
          background: linear-gradient(135deg, hsla(200, 80%, 50%, 0.4) 0%, hsla(200, 80%, 30%, 0.5) 100%);
          border-color: hsla(200, 80%, 70%, 0.6);
          transform: translateY(-1px);
          box-shadow: 0 4px 8px rgba(0,0,0,0.3);
        }
        .badge.atom { color: var(--primary); border: 1px solid var(--primary); }

        .sidebar-id {
          display: flex;
          flex-direction: column;
          gap: 4px;
          font-family: monospace;
          color: var(--text-muted);
          font-size: 0.8rem;
          word-break: break-all;
        }

        .sidebar-section {
          display: flex;
          flex-direction: column;
          gap: 8px;
        }

        .sidebar-section label {
          font-size: 0.7rem;
          text-transform: uppercase;
          color: var(--text-muted);
          font-weight: 700;
          letter-spacing: 0.05em;
        }

        .sidebar-section p, .sidebar-section span {
          margin: 0;
          line-height: 1.5;
          font-size: 0.95rem;
        }

        .content-box pre {
          margin: 0;
          white-space: pre-wrap;
          font-family: inherit;
          font-size: 0.85rem;
          color: var(--text);
          background: hsla(230, 20%, 5%, 0.5);
          padding: 12px;
          border-radius: 8px;
          border: 1px solid var(--surface-border);
          max-height: 400px;
          overflow-y: auto;
        }

        .json-dump {
          background: #000;
          padding: 12px;
          border-radius: 8px;
          font-size: 0.75rem;
          color: #00ff88;
          overflow-x: auto;
          border: 1px solid var(--surface-border);
        }

        .wellness-grid, .education-grid {
          display: grid;
          grid-template-columns: repeat(2, 1fr);
          gap: 12px;
          margin-top: 4px;
        }

        .wellness-metric, .education-metric {
          display: flex;
          flex-direction: column;
          gap: 4px;
          padding: 10px;
          background: hsla(230, 20%, 8%, 0.4);
          border-radius: 8px;
          border: 1px solid var(--surface-border);
          box-shadow: inset 0 1px 3px rgba(0,0,0,0.2);
        }

        .wellness-metric.full-width, .education-metric.full-width {
          grid-column: span 2;
        }

        .metric-val {
          font-size: 1.15rem;
          font-weight: 700;
          color: var(--primary);
        }

        .metric-val.activity { color: hsla(330, 90%, 65%, 1); }
        .metric-val.platform { color: hsla(45, 95%, 60%, 1); font-size: 1rem; }
        .metric-val.type { color: hsla(180, 80%, 60%, 1); font-size: 0.9rem; text-transform: capitalize; }
        .metric-val.credential { font-family: monospace; font-size: 0.8rem; color: var(--text-muted); }

        .metric-lbl {
          font-size: 0.65rem;
          text-transform: uppercase;
          color: var(--text-muted);
          letter-spacing: 0.05em;
          font-weight: 600;
        }
      `}</style>
    </div>
  );
}

