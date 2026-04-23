"use client";
import { useEffect, useRef, useState } from "react";

/**
 * @brief SWEBOK Knowledge Area Color Mapping
 */
const getKAColor = (ka) => {
  const colors = {
    1:  "hsla(350, 80%, 50%, 0.8)",  // Requirements (Red)
    2:  "hsla(180, 80%, 50%, 0.8)",  // Design (Cyan)
    3:  "hsla(210, 80%, 50%, 0.8)",  // Construction (Blue)
    4:  "hsla(45, 90%, 50%, 0.8)",   // Testing (Amber)
    5:  "hsla(280, 60%, 50%, 0.8)",  // Maintenance (Purple)
    6:  "hsla(120, 60%, 50%, 0.8)",  // Config Mgmt (Green)
    7:  "hsla(30, 80%, 50%, 0.8)",   // Eng Mgmt (Orange)
    8:  "hsla(240, 60%, 70%, 0.8)",  // Process (Lavender)
    9:  "hsla(190, 40%, 60%, 0.8)",  // Models (Steel)
    10: "hsla(150, 80%, 40%, 0.8)",  // Quality (Emerald)
    11: "hsla(0, 0%, 70%, 0.8)",     // Professional (Gray)
    12: "hsla(60, 80%, 50%, 0.8)",   // Economics (Yellow)
    13: "hsla(230, 80%, 40%, 0.8)",  // Computing Foundations
    14: "hsla(260, 80%, 40%, 0.8)",  // Math Foundations
    15: "hsla(200, 80%, 40%, 0.8)",  // Eng Foundations
  };
  return colors[ka] || "hsla(230, 20%, 40%, 0.8)";
};

const getKAName = (ka) => {
  const names = {
    1: "Requirements", 2: "Design", 3: "Construction", 4: "Testing",
    5: "Maintenance", 6: "Config Mgmt", 7: "Eng Mgmt", 8: "Eng Process",
    9: "Eng Models", 10: "Quality", 11: "Professional", 12: "Economics",
    13: "Computing", 14: "Math", 15: "Engineering"
  };
  return names[ka] || "Unknown";
};

/**
 * @brief Dynamic SVG Force-Directed Graph for ASOS Substrate
 */
export default function GraphMap({ data, selectedNodeId, onNodeSelect }) {
  const [nodes, setNodes] = useState([]);
  const [edges, setEdges] = useState([]);
  const [viewport, setViewport] = useState({ x: 0, y: 0, k: 1 });
  const [alpha, setAlpha] = useState(1); // Simulation Heat (Cooling factor)
  const [isPanning, setIsPanning] = useState(false);
  const [draggedNodeId, setDraggedNodeId] = useState(null);
  const requestRef = useRef();
  const containerRef = useRef();

  useEffect(() => {
    if (!data || !data.nodes) return;

    setAlpha(1); // Re-heat simulation on data change
    
    const initializedNodes = data.nodes.map((n) => {
      const existing = nodes.find(en => en.id === n.id);
      if (existing) return { ...n, x: existing.x, y: existing.y, vx: existing.vx, vy: existing.vy };
      
      // Projects start at corners to act as anchors
      const isProject = n.type === "PROJECT";
      return {
        ...n,
        x: isProject ? (Math.random() > 0.5 ? 400 : -400) : (Math.random() - 0.5) * 200,
        y: isProject ? (Math.random() > 0.5 ? 300 : -300) : (Math.random() - 0.5) * 200,
        vx: 0,
        vy: 0
      };
    });

    setNodes(initializedNodes);
    setEdges(data.edges);

    const simulate = () => {
      setAlpha(a => {
        // If dragging, keep the simulation hot
        if (draggedNodeId) return Math.max(a, 0.5);
        const nextAlpha = a * 0.992; // Linear-ish cooling
        if (nextAlpha < 0.01) return 0.01;
        return nextAlpha;
      });

      setNodes(prevNodes => {
        const nextNodes = prevNodes.map(node => ({ ...node }));
        
        // 1. Repulsion (Adaptive Cooling)
        for (let i = 0; i < nextNodes.length; i++) {
          for (let j = i + 1; j < nextNodes.length; j++) {
            const dx = nextNodes[j].x - nextNodes[i].x;
            const dy = nextNodes[j].y - nextNodes[i].y;
            const distSq = dx * dx + dy * dy || 1;
            const dist = Math.sqrt(distSq);
            
            // Softened repulsion for denser graphs
            const repulsionBase = nextNodes[i].type === "PROJECT" ? 4000 : 1000;
            const force = (repulsionBase * alpha) / distSq;
            
            const fx = (dx / dist) * force;
            const fy = (dy / dist) * force;
            
            nextNodes[i].vx -= fx;
            nextNodes[i].vy -= fy;
            nextNodes[j].vx += fx;
            nextNodes[j].vy += fy;

            const minPadding = nextNodes[i].type === "PROJECT" ? 80 : 40;
            if (dist < minPadding) {
              // Greatly reduced collision strength to prevent explosion
              const strength = (minPadding - dist) * 0.1;
              nextNodes[i].vx -= (dx / dist) * strength;
              nextNodes[i].vy -= (dy / dist) * strength;
              nextNodes[j].vx += (dx / dist) * strength;
              nextNodes[j].vy += (dy / dist) * strength;
            }
          }
        }

        // 2. Attraction
        data.edges.forEach(edge => {
          const source = nextNodes.find(n => n.id === edge.source);
          const target = nextNodes.find(n => n.id === edge.target);
          if (source && target) {
            const dx = target.x - source.x;
            const dy = target.y - source.y;
            const dist = Math.sqrt(dx * dx + dy * dy) || 1;
            const isAnchorEdge = target.type === "IDENTITY" || target.type === "PROJECT";
            const l = isAnchorEdge ? 80 : 100;
            const strength = isAnchorEdge ? 0.1 : 0.05;
            const force = (dist - l) * strength * alpha;
            const fx = (dx / dist) * force;
            const fy = (dy / dist) * force;
            source.vx += fx;
            source.vy += fy;
            target.vx -= fx;
            target.vy -= fy;
          }
        });

        // 3. Integration
        nextNodes.forEach(node => {
          // If this node is being dragged, lock its position and zero velocity
          if (node.id === draggedNodeId) {
            node.vx = 0;
            node.vy = 0;
            return;
          }

          node.vx -= node.x * (0.01 * alpha); // Gravity towards center
          node.vy -= node.y * (0.01 * alpha);
          
          // Speed limit to prevent catastrophic physics breaks
          const maxSpeed = 10;
          const speed = Math.sqrt(node.vx * node.vx + node.vy * node.vy);
          if (speed > maxSpeed) {
            node.vx = (node.vx / speed) * maxSpeed;
            node.vy = (node.vy / speed) * maxSpeed;
          }

          node.x += node.vx;
          node.y += node.vy;

          // Higher damping (lower multiplier) to encourage settling
          const damping = (node.type === "PROJECT" || node.type === "IDENTITY") ? 0.4 : 0.5;
          node.vx *= damping;
          node.vy *= damping;
        });

        return nextNodes;
      });
      requestRef.current = requestAnimationFrame(simulate);
    };

    requestRef.current = requestAnimationFrame(simulate);
    return () => cancelAnimationFrame(requestRef.current);
  }, [data, draggedNodeId, alpha]); // Added dependencies

  const handleWheel = (e) => {
    e.preventDefault();
    const scaleFactor = 1.1;
    const direction = e.deltaY > 0 ? 1 / scaleFactor : scaleFactor;
    setViewport(v => ({ ...v, k: Math.max(0.1, Math.min(5, v.k * direction)) }));
  };

  const getMousePos = (e) => {
    const rect = containerRef.current.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    const gx = (x - (400 + viewport.x)) / viewport.k;
    const gy = (y - (300 + viewport.y)) / viewport.k;
    return { gx, gy };
  };

  const handleMouseDown = (e) => {
    const { gx, gy } = getMousePos(e);
    const clickedNode = nodes.find(n => {
      const dx = n.x - gx;
      const dy = n.y - gy;
      const radius = n.type === "PROJECT" ? 25 : 15;
      return (dx * dx + dy * dy) < (radius * radius);
    });

    if (clickedNode) {
      setDraggedNodeId(clickedNode.id);
      if (onNodeSelect) onNodeSelect(clickedNode);
    } else {
      setIsPanning(true);
      if (onNodeSelect) onNodeSelect(null);
    }
  };

  const handleMouseUp = () => {
    setIsPanning(false);
    setDraggedNodeId(null);
  };

  const handleMouseMove = (e) => {
    if (draggedNodeId) {
      const { gx, gy } = getMousePos(e);
      setNodes(prev => prev.map(n => {
        if (n.id === draggedNodeId) {
          return { ...n, x: gx, y: gy, vx: 0, vy: 0 };
        }
        return n;
      }));
    } else if (isPanning) {
      setViewport(v => ({
        ...v,
        x: v.x + e.movementX,
        y: v.y + e.movementY
      }));
    }
  };

  // Node Renderer Helper
  const renderNode = (node) => {
    if (node.type === "PROJECT") {
      return (
        <g className="node-project">
          {/* Hexagon Shape */}
          <path
            d="M 0,-20 L 17.3,-10 L 17.3,10 L 0,20 L -17.3,10 L -17.3,-10 Z"
            fill="hsla(260, 60%, 20%, 0.9)"
            stroke="hsla(260, 60%, 60%, 1)"
            strokeWidth="3"
            filter="url(#glow)"
          />
          <path
            d="M 0,-12 L 10.4,-6 L 10.4,6 L 0,12 L -10.4,6 L -10.4,-6 Z"
            fill="none"
            stroke="var(--primary)"
            strokeWidth="1"
            opacity="0.5"
          />
        </g>
      );
    }
    if (node.type === "IDENTITY") {
      return (
        <g className="node-identity">
          {/* Diamond / Shield Shape */}
          <path
            d="M 0,-18 L 14,0 L 0,18 L -14,0 Z"
            fill="hsla(150, 60%, 20%, 0.9)"
            stroke="hsla(150, 60%, 60%, 1)"
            strokeWidth="3"
            filter="url(#glow)"
          />
          <circle r="4" fill="var(--secondary)" opacity="0.8" />
        </g>
      );
    }
    
    const isMilestone = node.tags?.includes("MILESTONE");
    const kaColor = getKAColor(node.ka);
    
    if (isMilestone) {
      return (
        <g className="node-milestone">
          <circle
            r="14"
            fill="hsla(45, 80%, 20%, 0.9)"
            stroke="hsla(45, 80%, 60%, 1)"
            strokeWidth="3"
            filter="url(#glow)"
          />
          <circle
            r="9"
            fill="none"
            stroke={kaColor}
            strokeWidth="1"
            opacity="0.8"
          />
          <circle r="4" fill="hsla(45, 80%, 60%, 1)" className="node-pulse" />
        </g>
      );
    }

    return (
      <g className="node-atom">
        <circle
          r="9"
          fill="hsla(230, 20%, 10%, 0.9)"
          stroke={kaColor}
          strokeWidth="2"
          filter="url(#glow)"
        />
        <circle
          r="4"
          fill={kaColor}
          className="node-pulse"
        />
      </g>
    );
  };

  return (
    <div 
      className="graph-canvas-wrapper glass" 
      ref={containerRef}
      onWheel={handleWheel}
      onMouseDown={handleMouseDown}
      onMouseUp={handleMouseUp}
      onMouseMove={handleMouseMove}
      onMouseLeave={handleMouseUp}
    >
      <svg width="100%" height="600" style={{ cursor: isPanning ? 'grabbing' : 'crosshair' }}>
        <defs>
          <pattern id="dotGrid" width="20" height="20" patternUnits="userSpaceOnUse">
            <circle cx="2" cy="2" r="0.8" fill="hsla(230, 20%, 60%, 0.5)" />
          </pattern>
          <filter id="glow">
            <feGaussianBlur stdDeviation="2.5" result="coloredBlur" />
            <feMerge>
              <feMergeNode in="coloredBlur" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
        </defs>

        <g transform={`translate(${400 + viewport.x}, ${300 + viewport.y}) scale(${viewport.k})`}>
          <rect x="-4000" y="-4000" width="8000" height="8000" fill="url(#dotGrid)" />

          {edges.map((edge, i) => {
            const source = nodes.find(n => n.id === edge.source);
            const target = nodes.find(n => n.id === edge.target);
            if (!source || !target) return null;

            let isHighlighted = false;
            let isDimmed = false;
            
            if (selectedNodeId) {
              if (edge.source === selectedNodeId || edge.target === selectedNodeId) {
                isHighlighted = true;
              } else {
                isDimmed = true;
              }
            }

            return (
              <line
                key={`edge-${i}`}
                x1={source.x} y1={source.y}
                x2={target.x} y2={target.y}
                stroke={edge.label === 'CREATED_BY' ? "var(--secondary)" : "var(--primary)"}
                strokeWidth={isHighlighted ? "3" : (edge.label === 'BELONGS_TO' ? "2" : "1")}
                opacity={isDimmed ? "0.1" : (isHighlighted ? "1" : (alpha > 0.5 ? "0.3" : "0.7"))}
                style={{ transition: 'opacity 0.3s ease, stroke-width 0.3s ease' }}
              />
            );
          })}

          {nodes.map((node) => {
            // Highlighting Logic
            const isSelected = node.id === selectedNodeId;
            let isHighlighted = isSelected;
            if (selectedNodeId && !isSelected) {
               // Check if it's a neighbor
               const isNeighbor = edges.some(e => 
                 (e.source === selectedNodeId && e.target === node.id) ||
                 (e.target === selectedNodeId && e.source === node.id)
               );
               if (isNeighbor) isHighlighted = true;
            }
            const isDimmed = selectedNodeId && !isHighlighted;

            return (
            <g 
              key={node.id} 
              transform={`translate(${node.x}, ${node.y})`}
              style={{ opacity: isDimmed ? 0.15 : 1, transition: 'opacity 0.3s ease' }}
            >
              {isSelected && (
                <circle r="35" fill="none" stroke="var(--primary)" strokeWidth="1" strokeDasharray="4 4" className="spin-slow" />
              )}
              {renderNode(node)}
              <text
                y="22"
                textAnchor="middle"
                fill={isHighlighted ? "var(--primary)" : "var(--text)"}
                fontSize="10"
                fontWeight="700"
                className="node-label"
                style={{ textShadow: '0 2px 4px rgba(0,0,0,0.8)' }}
              >
                {node.label || node.id}
              </text>
            </g>
          )})}
        </g>
      </svg>

      <div className="graph-legend glass">
        <h4>Substrate Legend</h4>
        <div className="legend-items">
          <div className="legend-item">
            <span className="shape project"></span>
            <span>Project</span>
          </div>
          <div className="legend-item">
            <span className="shape identity"></span>
            <span>Identity</span>
          </div>
          <div className="legend-item">
            <span className="shape milestone"></span>
            <span>Milestone</span>
          </div>
          <div className="legend-divider"></div>
          {[1, 2, 4, 6, 10].map(ka => (
            <div key={ka} className="legend-item">
              <span className="dot" style={{ background: getKAColor(ka) }}></span>
              <span>{getKAName(ka)}</span>
            </div>
          ))}
        </div>
      </div>



      <style jsx>{`
        .graph-canvas-wrapper {
          width: 100%;
          height: 600px;
          background: #05070a; /* Ensure a solid base for visibility */
          background-image: radial-gradient(circle at center, hsla(230, 20%, 15%, 0.4) 0%, transparent 70%);
          overflow: hidden;
          cursor: crosshair;
        }

        .node-pulse {
          animation: nodePulse 4s infinite ease-in-out;
        }

        @keyframes nodePulse {
          0%, 100% { opacity: 0.8; transform: scale(1); }
          50% { opacity: 1; transform: scale(1.1); }
        }

        .spin-slow {
          animation: spinSlow 10s linear infinite;
        }

        @keyframes spinSlow {
          to { transform: rotate(360deg); }
        }

        .node-label {
          pointer-events: none;
          text-transform: uppercase;
          letter-spacing: 0.05em;
          font-weight: 700;
        }

        .graph-legend {
          position: absolute;
          bottom: 20px;
          left: 20px;
          padding: 16px;
          width: 180px;
          pointer-events: none;
          display: flex;
          flex-direction: column;
          gap: 12px;
        }

        .graph-legend h4 {
          margin: 0;
          font-size: 0.7rem;
          text-transform: uppercase;
          color: var(--text-muted);
          letter-spacing: 0.1em;
        }

        .legend-items {
          display: flex;
          flex-direction: column;
          gap: 8px;
        }

        .legend-item {
          display: flex;
          align-items: center;
          gap: 10px;
          font-size: 0.7rem;
          font-weight: 600;
          color: var(--text);
        }

        .legend-divider {
          height: 1px;
          background: var(--surface-border);
          margin: 4px 0;
        }

        .shape {
          width: 12px;
          height: 12px;
          border: 1.5px solid currentColor;
        }

        .shape.project {
          clip-path: polygon(50% 0%, 100% 25%, 100% 75%, 50% 100%, 0% 75%, 0% 25%);
          background: hsla(260, 60%, 40%, 0.5);
          color: hsla(260, 60%, 60%, 1);
        }

        .shape.identity {
          transform: rotate(45deg);
          background: hsla(150, 60%, 40%, 0.5);
          color: hsla(150, 60%, 60%, 1);
        }

        .shape.milestone {
          border-radius: 50%;
          background: hsla(45, 80%, 40%, 0.5);
          color: hsla(45, 80%, 60%, 1);
          border-width: 2px;
        }

        .dot {
          width: 8px;
          height: 8px;
          border-radius: 50%;
        }

        .node-project:hover path, .node-identity:hover path, .node-atom:hover circle {
          stroke-width: 4;
          filter: brightness(1.5) url(#glow);
        }
      `}</style>
    </div>
  );
}
