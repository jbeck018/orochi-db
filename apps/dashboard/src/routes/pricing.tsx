import * as React from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import { Check, HelpCircle, ArrowRight } from "lucide-react";
import howleropsLogo from "@/src/assets/howlerops-icon.png";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardFooter,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip";

export const Route = createFileRoute("/pricing")({
  component: PricingPage,
});

const tiers = [
  {
    name: "Free",
    price: "$0",
    description: "Perfect for learning and small projects",
    features: [
      { text: "1 cluster", tooltip: "Single development cluster" },
      { text: "1 GB storage", tooltip: "Included storage per cluster" },
      { text: "100 MB RAM", tooltip: "Shared compute resources" },
      { text: "Community support", tooltip: "GitHub discussions and community forums" },
      { text: "Daily backups", tooltip: "Automated daily backup retention" },
      { text: "Basic monitoring", tooltip: "CPU, memory, and storage metrics" },
    ],
    cta: "Get Started Free",
    popular: false,
    ctaVariant: "outline" as const,
  },
  {
    name: "Standard",
    price: "$29",
    period: "/month",
    description: "For growing applications and teams",
    features: [
      { text: "5 clusters", tooltip: "Up to 5 production clusters" },
      { text: "50 GB storage", tooltip: "Included storage per cluster" },
      { text: "2 GB RAM", tooltip: "Dedicated compute resources" },
      { text: "Email support", tooltip: "48-hour response time" },
      { text: "Point-in-time recovery", tooltip: "Restore to any second within retention" },
      { text: "Advanced monitoring", tooltip: "Query performance, replication lag, and custom metrics" },
      { text: "Auto-scaling", tooltip: "Automatic resource scaling based on load" },
      { text: "2 read replicas", tooltip: "Read replicas for scaling reads" },
    ],
    cta: "Start Free Trial",
    popular: true,
    ctaVariant: "default" as const,
  },
  {
    name: "Professional",
    price: "$149",
    period: "/month",
    description: "For production workloads at scale",
    features: [
      { text: "Unlimited clusters", tooltip: "No cluster limits" },
      { text: "500 GB storage", tooltip: "Included storage per cluster" },
      { text: "16 GB RAM", tooltip: "High-performance compute" },
      { text: "Priority support", tooltip: "4-hour response time" },
      { text: "Multi-region replication", tooltip: "Global data distribution" },
      { text: "Advanced security", tooltip: "VPC peering, IP allowlists, and encryption" },
      { text: "99.95% SLA", tooltip: "Guaranteed uptime with credits" },
      { text: "10 read replicas", tooltip: "Scale reads globally" },
      { text: "Custom retention", tooltip: "Configure backup retention policies" },
    ],
    cta: "Start Free Trial",
    popular: false,
    ctaVariant: "outline" as const,
  },
  {
    name: "Enterprise",
    price: "Custom",
    description: "For large organizations with custom needs",
    features: [
      { text: "Unlimited everything", tooltip: "No limits on resources" },
      { text: "Dedicated infrastructure", tooltip: "Isolated compute and storage" },
      { text: "Custom RAM/CPU", tooltip: "Tailored to your workload" },
      { text: "24/7 phone support", tooltip: "Direct line to engineering team" },
      { text: "99.99% SLA", tooltip: "Enterprise-grade uptime guarantee" },
      { text: "Custom integrations", tooltip: "SSO, LDAP, and custom connectors" },
      { text: "Dedicated account manager", tooltip: "Your personal Orochi expert" },
      { text: "On-premise option", tooltip: "Deploy in your own infrastructure" },
      { text: "Compliance certifications", tooltip: "SOC 2, HIPAA, GDPR support" },
    ],
    cta: "Contact Sales",
    popular: false,
    ctaVariant: "outline" as const,
  },
];

const faqs = [
  {
    question: "Can I change plans at any time?",
    answer:
      "Yes, you can upgrade or downgrade your plan at any time. When upgrading, you'll be charged the prorated difference. When downgrading, the credit will be applied to future invoices.",
  },
  {
    question: "What payment methods do you accept?",
    answer:
      "We accept all major credit cards (Visa, MasterCard, American Express) and can arrange invoicing for Enterprise customers.",
  },
  {
    question: "Is there a free trial?",
    answer:
      "Yes! All paid plans include a 14-day free trial with full access to all features. No credit card required to start.",
  },
  {
    question: "What happens if I exceed my storage limit?",
    answer:
      "We'll notify you when you reach 80% of your storage limit. You can upgrade your plan or add additional storage at $0.10/GB/month.",
  },
  {
    question: "Do you offer discounts for annual billing?",
    answer:
      "Yes, annual billing includes a 20% discount compared to monthly billing. Contact sales for multi-year agreements.",
  },
  {
    question: "What's included in the SLA?",
    answer:
      "Our SLA covers database availability and API uptime. If we fail to meet the SLA, you'll receive service credits automatically applied to your account.",
  },
];

function PricingPage(): React.JSX.Element {
  return (
    <TooltipProvider>
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
                className="text-sm font-medium text-foreground"
              >
                Pricing
              </Link>
              <Link
                to="/docs"
                className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
              >
                Documentation
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

        {/* Header */}
        <section className="container mx-auto px-4 py-16 text-center">
          <h1 className="text-4xl md:text-5xl font-bold tracking-tight mb-4">
            Simple, Transparent Pricing
          </h1>
          <p className="text-xl text-muted-foreground max-w-2xl mx-auto">
            Choose the plan that fits your needs. All plans include full
            PostgreSQL compatibility, automatic backups, and monitoring.
          </p>
        </section>

        {/* Pricing Cards */}
        <section className="container mx-auto px-4 pb-16">
          <div className="grid gap-6 md:grid-cols-2 lg:grid-cols-4">
            {tiers.map((tier) => (
              <Card
                key={tier.name}
                className={`relative flex flex-col ${
                  tier.popular
                    ? "border-primary shadow-lg scale-105"
                    : ""
                }`}
              >
                {tier.popular && (
                  <div className="absolute -top-3 left-1/2 -translate-x-1/2">
                    <span className="bg-primary text-primary-foreground text-xs font-medium px-3 py-1 rounded-full">
                      Most Popular
                    </span>
                  </div>
                )}
                <CardHeader>
                  <CardTitle>{tier.name}</CardTitle>
                  <CardDescription>{tier.description}</CardDescription>
                  <div className="mt-4">
                    <span className="text-4xl font-bold">{tier.price}</span>
                    {tier.period && (
                      <span className="text-muted-foreground">{tier.period}</span>
                    )}
                  </div>
                </CardHeader>
                <CardContent className="flex-1">
                  <ul className="space-y-3">
                    {tier.features.map((feature, index) => (
                      <li key={index} className="flex items-start gap-2">
                        <Check className="h-5 w-5 text-primary flex-shrink-0 mt-0.5" />
                        <span className="text-sm">{feature.text}</span>
                        <Tooltip>
                          <TooltipTrigger asChild>
                            <HelpCircle className="h-4 w-4 text-muted-foreground flex-shrink-0 cursor-help" />
                          </TooltipTrigger>
                          <TooltipContent>
                            <p className="max-w-xs">{feature.tooltip}</p>
                          </TooltipContent>
                        </Tooltip>
                      </li>
                    ))}
                  </ul>
                </CardContent>
                <CardFooter>
                  <Button
                    className="w-full"
                    variant={tier.ctaVariant}
                    asChild
                  >
                    <Link to={tier.name === "Enterprise" ? "/register" : "/register"}>
                      {tier.cta}
                      {tier.name !== "Enterprise" && (
                        <ArrowRight className="ml-2 h-4 w-4" />
                      )}
                    </Link>
                  </Button>
                </CardFooter>
              </Card>
            ))}
          </div>
        </section>

        {/* Feature Comparison */}
        <section className="border-t bg-muted/50 py-16">
          <div className="container mx-auto px-4">
            <h2 className="text-3xl font-bold tracking-tight text-center mb-12">
              Compare Plans
            </h2>
            <div className="overflow-x-auto">
              <table className="w-full border-collapse">
                <thead>
                  <tr className="border-b">
                    <th className="text-left p-4 font-medium">Feature</th>
                    <th className="text-center p-4 font-medium">Free</th>
                    <th className="text-center p-4 font-medium bg-primary/5">
                      Standard
                    </th>
                    <th className="text-center p-4 font-medium">Professional</th>
                    <th className="text-center p-4 font-medium">Enterprise</th>
                  </tr>
                </thead>
                <tbody>
                  {[
                    ["Clusters", "1", "5", "Unlimited", "Unlimited"],
                    ["Storage", "1 GB", "50 GB", "500 GB", "Custom"],
                    ["RAM", "100 MB", "2 GB", "16 GB", "Custom"],
                    ["Read Replicas", "-", "2", "10", "Unlimited"],
                    ["Backups", "Daily", "PITR", "PITR", "PITR + Custom"],
                    ["Support", "Community", "Email", "Priority", "24/7 Phone"],
                    ["SLA", "-", "-", "99.95%", "99.99%"],
                    ["Multi-region", "-", "-", "Yes", "Yes"],
                    ["VPC Peering", "-", "-", "Yes", "Yes"],
                    ["SSO/SAML", "-", "-", "-", "Yes"],
                  ].map((row, index) => (
                    <tr key={index} className="border-b">
                      <td className="p-4 font-medium">{row[0]}</td>
                      <td className="text-center p-4">{row[1]}</td>
                      <td className="text-center p-4 bg-primary/5">{row[2]}</td>
                      <td className="text-center p-4">{row[3]}</td>
                      <td className="text-center p-4">{row[4]}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
        </section>

        {/* FAQ Section */}
        <section className="py-16">
          <div className="container mx-auto px-4">
            <h2 className="text-3xl font-bold tracking-tight text-center mb-12">
              Frequently Asked Questions
            </h2>
            <div className="grid gap-6 md:grid-cols-2 max-w-4xl mx-auto">
              {faqs.map((faq, index) => (
                <Card key={index}>
                  <CardHeader>
                    <CardTitle className="text-lg">{faq.question}</CardTitle>
                  </CardHeader>
                  <CardContent>
                    <p className="text-muted-foreground">{faq.answer}</p>
                  </CardContent>
                </Card>
              ))}
            </div>
          </div>
        </section>

        {/* CTA Section */}
        <section className="border-t bg-primary text-primary-foreground py-16">
          <div className="container mx-auto px-4 text-center">
            <h2 className="text-3xl font-bold tracking-tight mb-4">
              Ready to Get Started?
            </h2>
            <p className="text-lg opacity-90 mb-8 max-w-2xl mx-auto">
              Start with our free tier and upgrade as you grow. No credit card
              required.
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
                <a href="mailto:sales@howlerops.com">Contact Sales</a>
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
              <p>
                &copy; {new Date().getFullYear()} HowlerOps. All rights
                reserved.
              </p>
            </div>
          </div>
        </footer>
      </div>
    </TooltipProvider>
  );
}
