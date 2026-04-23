import "./globals.css";
import Sidebar from "../components/Sidebar";


export const metadata = {
  title: "ASOS Agentic Dashboard",
  description: "Distributed Knowledge Swarm Governance",
};

export default function RootLayout({ children }) {
  return (
    <html lang="en">
      <body>
        <div className="layout-container">
          <div className="noise-overlay"></div>
          <Sidebar />
          
          <div className="main-wrapper">

            <header className="top-bar glass">
              <div className="substrate-status">
                <span className="status-dot online"></span>
                <span className="status-text">SUBSTRATE ONLINE</span>
                <span className="status-divider">|</span>
                <span className="status-metric">HEALTH: 100%</span>
                <span className="status-divider">|</span>
                <span className="status-metric">LATENCY: 12ms</span>
              </div>
              <div className="user-profile">
                <span className="agent-id">Agent v0.4-α</span>
                <div className="avatar"></div>
              </div>
            </header>
            
            <main className="content">
              {children}
            </main>
          </div>
        </div>

      </body>
    </html>
  );
}

