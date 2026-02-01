import * as React from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import {
  Database,
  Zap,
  Clock,
  BarChart3,
  Layers,
  GitBranch,
  Copy,
  ArrowRight,
  Check,
  ChevronRight,
  Network,
  Activity,
} from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import howleropsLogo from "@/src/assets/howlerops-icon.png";

export const Route = createFileRoute("/landing")({
  component: LandingPage,
});

const features = [
  {
    icon: Database,
    title: "Automatic Sharding",
    description:
      "Hash-based horizontal distribution across nodes. Scale your data effortlessly without manual intervention.",
  },
  {
    icon: Clock,
    title: "Time-Series Optimization",
    description:
      "Automatic time-based partitioning with chunks for efficient time-series workloads.",
  },
  {
    icon: Layers,
    title: "Columnar Storage",
    description:
      "Column-oriented format with advanced compression for blazing-fast analytics queries.",
  },
  {
    icon: BarChart3,
    title: "Tiered Storage",
    description:
      "Hot/warm/cold/frozen data lifecycle with S3 integration for cost-effective storage.",
  },
  {
    icon: Zap,
    title: "Vector & AI Workloads",
    description:
      "SIMD-optimized vector operations and similarity search for AI/ML applications.",
  },
  {
    icon: GitBranch,
    title: "Change Data Capture",
    description:
      "Stream database changes to external systems in real-time with CDC support.",
  },
  {
    icon: Copy,
    title: "Instant Database Branching",
    description:
      "Create instant copies of your database for development, testing, and staging in seconds using copy-on-write technology.",
  },
  {
    icon: Network,
    title: "Intelligent Connection Pooling",
    description:
      "Built-in PgDog connection pooler with automatic query routing, read/write splitting, and scale-to-zero support.",
  },
  {
    icon: Activity,
    title: "Performance Benchmarks",
    description:
      "Comprehensive benchmark suite with real-time dashboards showing TPC-H, time-series, and columnar storage performance.",
  },
];

const useCases = [
  {
    title: "Real-Time Analytics",
    description:
      "Run complex analytical queries on live transactional data without impacting your OLTP workloads.",
    icon: BarChart3,
  },
  {
    title: "Time-Series Data",
    description:
      "Store and query billions of time-series data points with automatic partitioning and compression.",
    icon: Clock,
  },
  {
    title: "AI/ML Applications",
    description:
      "Build intelligent applications with native vector search and similarity matching capabilities.",
    icon: Zap,
  },
];

function LandingPage(): React.JSX.Element {
  return (
    <div className="min-h-screen bg-background">
      {/* Navigation */}
      <nav className="border-b bg-background/95 backdrop-blur supports-[backdrop-filter]:bg-background/60 sticky top-0 z-50">
        <div className="container mx-auto flex h-16 items-center justify-between px-4">
          <Link to="/" className="flex items-center gap-2">
            <img src={howleropsLogo} alt="HowlerOps" className="h-10 w-10" />
            <span className="text-xl font-bold">HowlerOps</span>
          </Link>
          <div className="hidden md:flex items-center gap-6">
            <Link
              to="/"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Features
            </Link>
            <Link
              to="/pricing"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Pricing
            </Link>
            <Link
              to="/docs"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Documentation
            </Link>
            <Link
              to="/benchmarks"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Benchmarks
            </Link>
          </div>
          <div className="flex items-center gap-4">
            <Button variant="ghost" asChild>
              <Link to="/login">Sign In</Link>
            </Button>
            <Button asChild>
              <Link to="/register">Get Started</Link>
            </Button>
          </div>
        </div>
      </nav>

      {/* Hero Section */}
      <section className="container mx-auto px-4 py-20 md:py-32">
        <div className="flex flex-col items-center text-center max-w-4xl mx-auto">
          <div className="inline-flex items-center gap-2 rounded-full border px-4 py-1.5 text-sm mb-6">
            <span className="text-primary font-medium">New</span>
            <span className="text-muted-foreground">
              Instant database branching now available
            </span>
            <ChevronRight className="h-4 w-4 text-muted-foreground" />
          </div>
          <h1 className="text-4xl md:text-6xl font-bold tracking-tight mb-6">
            <span className="text-primary">OrochiDB</span> - PostgreSQL for{" "}
            Modern Workloads
          </h1>
          <p className="text-xl text-muted-foreground mb-8 max-w-2xl">
            OrochiDB combines OLTP and OLAP in a single PostgreSQL database.
            Automatic sharding, time-series optimization, columnar storage, and
            vector search - fully managed by HowlerOps.
          </p>
          <div className="flex flex-col sm:flex-row gap-4">
            <Button size="lg" asChild>
              <Link to="/register">
                Start Free Trial
                <ArrowRight className="ml-2 h-4 w-4" />
              </Link>
            </Button>
            <Button size="lg" variant="outline" asChild>
              <Link to="/docs">View Documentation</Link>
            </Button>
          </div>
          <p className="text-sm text-muted-foreground mt-4">
            No credit card required. Free tier includes 1GB storage.
          </p>
        </div>
      </section>

      {/* Features Grid */}
      <section className="border-t bg-muted/50 py-20">
        <div className="container mx-auto px-4">
          <div className="text-center mb-12">
            <h2 className="text-3xl font-bold tracking-tight mb-4">
              Everything You Need for HTAP Workloads
            </h2>
            <p className="text-lg text-muted-foreground max-w-2xl mx-auto">
              Built on PostgreSQL with extensions for modern hybrid
              transactional/analytical processing.
            </p>
          </div>
          <div className="grid gap-6 md:grid-cols-2 lg:grid-cols-3">
            {features.map((feature) => (
              <Card key={feature.title} className="bg-background">
                <CardHeader>
                  <feature.icon className="h-10 w-10 text-primary mb-2" />
                  <CardTitle>{feature.title}</CardTitle>
                </CardHeader>
                <CardContent>
                  <CardDescription className="text-base">
                    {feature.description}
                  </CardDescription>
                </CardContent>
              </Card>
            ))}
          </div>
        </div>
      </section>

      {/* Use Cases */}
      <section className="py-20">
        <div className="container mx-auto px-4">
          <div className="text-center mb-12">
            <h2 className="text-3xl font-bold tracking-tight mb-4">
              Built for Your Use Case
            </h2>
            <p className="text-lg text-muted-foreground max-w-2xl mx-auto">
              Whether you need real-time analytics, time-series storage, or AI
              capabilities, OrochiDB has you covered.
            </p>
          </div>
          <div className="grid gap-8 md:grid-cols-3">
            {useCases.map((useCase) => (
              <div
                key={useCase.title}
                className="flex flex-col items-center text-center p-6"
              >
                <div className="h-16 w-16 rounded-full bg-primary/10 flex items-center justify-center mb-4">
                  <useCase.icon className="h-8 w-8 text-primary" />
                </div>
                <h3 className="text-xl font-semibold mb-2">{useCase.title}</h3>
                <p className="text-muted-foreground">{useCase.description}</p>
              </div>
            ))}
          </div>
        </div>
      </section>

      {/* Stats Section */}
      <section className="border-t border-b bg-muted/50 py-16">
        <div className="container mx-auto px-4">
          <div className="grid gap-8 md:grid-cols-4 text-center">
            <div>
              <div className="text-4xl font-bold text-primary mb-2">99.99%</div>
              <div className="text-muted-foreground">Uptime SLA</div>
            </div>
            <div>
              <div className="text-4xl font-bold text-primary mb-2">10x</div>
              <div className="text-muted-foreground">Faster Analytics</div>
            </div>
            <div>
              <div className="text-4xl font-bold text-primary mb-2">80%</div>
              <div className="text-muted-foreground">Storage Savings</div>
            </div>
            <div>
              <div className="text-4xl font-bold text-primary mb-2">24/7</div>
              <div className="text-muted-foreground">Expert Support</div>
            </div>
          </div>
        </div>
      </section>

      {/* Why Orochi */}
      <section className="py-20">
        <div className="container mx-auto px-4">
          <div className="grid gap-12 md:grid-cols-2 items-center">
            <div>
              <h2 className="text-3xl font-bold tracking-tight mb-6">
                Why Choose OrochiDB?
              </h2>
              <div className="space-y-4">
                {[
                  "Full PostgreSQL compatibility - use your existing tools and queries",
                  "Instant database branching for dev/test environments",
                  "Built-in PgDog connection pooler with intelligent query routing",
                  "Automatic scaling based on workload demands",
                  "Enterprise-grade security with encryption at rest and in transit",
                  "Multi-region deployments for global applications",
                  "Integrated monitoring and alerting",
                  "Instant point-in-time recovery",
                  "Comprehensive performance benchmarks and analytics",
                ].map((item, index) => (
                  <div key={index} className="flex items-start gap-3">
                    <Check className="h-5 w-5 text-primary mt-0.5 flex-shrink-0" />
                    <span>{item}</span>
                  </div>
                ))}
              </div>
              <Button className="mt-8" asChild>
                <Link to="/register">
                  Start Building Today
                  <ArrowRight className="ml-2 h-4 w-4" />
                </Link>
              </Button>
            </div>
            <div className="relative">
              <div className="bg-muted rounded-lg p-6 font-mono text-sm">
                <div className="text-muted-foreground mb-2">
                  -- Create a distributed hypertable
                </div>
                <div className="text-foreground">
                  <span className="text-primary">SELECT</span> create_hypertable(
                </div>
                <div className="text-foreground pl-4">
                  <span className="text-yellow-500">'events'</span>,
                </div>
                <div className="text-foreground pl-4">
                  <span className="text-yellow-500">'timestamp'</span>,
                </div>
                <div className="text-foreground pl-4">
                  chunk_time_interval =&gt;{" "}
                  <span className="text-yellow-500">INTERVAL '1 day'</span>
                </div>
                <div className="text-foreground">);</div>
                <div className="text-muted-foreground mt-4 mb-2">
                  -- Enable columnar compression
                </div>
                <div className="text-foreground">
                  <span className="text-primary">ALTER TABLE</span> events
                </div>
                <div className="text-foreground pl-4">
                  <span className="text-primary">SET</span> (
                </div>
                <div className="text-foreground pl-8">
                  orochi.columnar_enabled ={" "}
                  <span className="text-green-500">true</span>,
                </div>
                <div className="text-foreground pl-8">
                  orochi.compression = <span className="text-yellow-500">'zstd'</span>
                </div>
                <div className="text-foreground pl-4">);</div>
              </div>
            </div>
          </div>
        </div>
      </section>

      {/* CTA Section */}
      <section className="border-t bg-primary text-primary-foreground py-20">
        <div className="container mx-auto px-4 text-center">
          <h2 className="text-3xl font-bold tracking-tight mb-4">
            Ready to Get Started?
          </h2>
          <p className="text-lg opacity-90 mb-8 max-w-2xl mx-auto">
            Deploy your first OrochiDB cluster in minutes. Start with our
            free tier and scale as you grow.
          </p>
          <div className="flex flex-col sm:flex-row gap-4 justify-center">
            <Button size="lg" variant="secondary" asChild>
              <Link to="/register">
                Create Free Account
                <ArrowRight className="ml-2 h-4 w-4" />
              </Link>
            </Button>
            <Button
              size="lg"
              variant="outline"
              className="bg-transparent border-primary-foreground text-primary-foreground hover:bg-primary-foreground/10"
              asChild
            >
              <Link to="/pricing">View Pricing</Link>
            </Button>
          </div>
        </div>
      </section>

      {/* Footer */}
      <footer className="border-t py-12">
        <div className="container mx-auto px-4">
          <div className="grid gap-8 md:grid-cols-4">
            <div>
              <div className="flex items-center gap-2 mb-4">
                <img src={howleropsLogo} alt="HowlerOps" className="h-8 w-8" />
                <span className="font-bold">HowlerOps</span>
              </div>
              <p className="text-sm text-muted-foreground">
                OrochiDB - The PostgreSQL platform for modern HTAP workloads.
              </p>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Product</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <Link to="/" className="hover:text-foreground">
                    Features
                  </Link>
                </li>
                <li>
                  <Link to="/pricing" className="hover:text-foreground">
                    Pricing
                  </Link>
                </li>
                <li>
                  <Link to="/docs" className="hover:text-foreground">
                    Documentation
                  </Link>
                </li>
                <li>
                  <Link to="/benchmarks" className="hover:text-foreground">
                    Benchmarks
                  </Link>
                </li>
              </ul>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Company</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <a href="#" className="hover:text-foreground">
                    About
                  </a>
                </li>
                <li>
                  <a href="#" className="hover:text-foreground">
                    Blog
                  </a>
                </li>
                <li>
                  <a href="#" className="hover:text-foreground">
                    Careers
                  </a>
                </li>
              </ul>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Legal</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <Link to="/privacy" className="hover:text-foreground">
                    Privacy Policy
                  </Link>
                </li>
                <li>
                  <Link to="/terms" className="hover:text-foreground">
                    Terms of Service
                  </Link>
                </li>
              </ul>
            </div>
          </div>
          <div className="border-t mt-8 pt-8 text-center text-sm text-muted-foreground">
            <p>&copy; {new Date().getFullYear()} HowlerOps. All rights reserved.</p>
          </div>
        </div>
      </footer>
    </div>
  );
}
