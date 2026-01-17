import { createFileRoute, Link } from "@tanstack/react-router";
import { ArrowLeft } from "lucide-react";
import { Button } from "@/components/ui/button";

export const Route = createFileRoute("/terms")({
  component: TermsOfServicePage,
});

function TermsOfServicePage(): React.JSX.Element {
  return (
    <div className="min-h-screen bg-background">
      <div className="mx-auto max-w-3xl px-4 py-12 sm:px-6 lg:px-8">
        <Link to="/">
          <Button variant="ghost" className="mb-8">
            <ArrowLeft className="mr-2 h-4 w-4" />
            Back to Home
          </Button>
        </Link>

        <h1 className="text-4xl font-bold tracking-tight mb-8">Terms of Service</h1>

        <div className="prose prose-slate dark:prose-invert max-w-none">
          <p className="text-muted-foreground">
            Last updated: January 16, 2026
          </p>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">1. Acceptance of Terms</h2>
            <p>
              By accessing or using Orochi Cloud (&quot;the Service&quot;), you agree to be bound by these Terms of Service and all applicable laws and regulations. If you do not agree with any of these terms, you are prohibited from using or accessing this Service.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">2. Description of Service</h2>
            <p>
              Orochi Cloud provides managed PostgreSQL database services with advanced features including:
            </p>
            <ul className="list-disc pl-6 mt-2 space-y-1">
              <li>Automatic sharding and distributed queries</li>
              <li>Time-series data optimization</li>
              <li>Columnar storage for analytics</li>
              <li>Vector/AI workload support</li>
              <li>Tiered storage with cloud integration</li>
              <li>Automatic backups and point-in-time recovery</li>
            </ul>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">3. Account Registration</h2>
            <p>
              To use the Service, you must create an account with accurate and complete information. You are responsible for maintaining the confidentiality of your account credentials and for all activities that occur under your account.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">4. Acceptable Use</h2>
            <p>You agree not to:</p>
            <ul className="list-disc pl-6 mt-2 space-y-1">
              <li>Use the Service for any unlawful purpose</li>
              <li>Store or transmit malicious code</li>
              <li>Attempt to gain unauthorized access to any systems</li>
              <li>Interfere with or disrupt the Service</li>
              <li>Resell or redistribute the Service without authorization</li>
              <li>Use the Service to send spam or unsolicited communications</li>
            </ul>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">5. Service Level Agreement</h2>
            <p>
              For paid tiers, we provide service level guarantees as specified in your subscription plan. The Free tier is provided &quot;as-is&quot; without uptime guarantees.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">6. Payment and Billing</h2>
            <p>
              Paid services are billed in advance on a monthly or annual basis. You authorize us to charge your payment method for all fees incurred. Failure to pay may result in service suspension.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">7. Data and Privacy</h2>
            <p>
              Your use of the Service is also governed by our <Link to="/privacy" className="text-primary hover:underline">Privacy Policy</Link>. You retain ownership of all data you store in the Service. We implement industry-standard security measures to protect your data.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">8. Termination</h2>
            <p>
              Either party may terminate service at any time. Upon termination, you may export your data for 30 days before it is permanently deleted. We may suspend or terminate accounts that violate these terms.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">9. Limitation of Liability</h2>
            <p>
              To the maximum extent permitted by law, Orochi Cloud shall not be liable for any indirect, incidental, special, consequential, or punitive damages resulting from your use of the Service.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">10. Changes to Terms</h2>
            <p>
              We reserve the right to modify these terms at any time. We will notify users of significant changes via email or through the Service. Continued use after changes constitutes acceptance of the new terms.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">11. Contact Information</h2>
            <p>
              For questions about these Terms of Service, please contact us at:
            </p>
            <p className="mt-2">
              <strong>Email:</strong>{" "}
              <a href="mailto:legal@orochi.dev" className="text-primary hover:underline">
                legal@orochi.dev
              </a>
            </p>
          </section>
        </div>
      </div>
    </div>
  );
}
