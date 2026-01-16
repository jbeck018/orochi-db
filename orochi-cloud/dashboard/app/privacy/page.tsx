import Link from "next/link";
import { ArrowLeft } from "lucide-react";
import { Button } from "@/components/ui/button";

export default function PrivacyPolicyPage(): React.JSX.Element {
  return (
    <div className="min-h-screen bg-background">
      <div className="mx-auto max-w-3xl px-4 py-12 sm:px-6 lg:px-8">
        <Link href="/">
          <Button variant="ghost" className="mb-8">
            <ArrowLeft className="mr-2 h-4 w-4" />
            Back to Home
          </Button>
        </Link>

        <h1 className="text-4xl font-bold tracking-tight mb-8">Privacy Policy</h1>

        <div className="prose prose-slate dark:prose-invert max-w-none">
          <p className="text-muted-foreground">
            Last updated: January 16, 2026
          </p>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">1. Introduction</h2>
            <p>
              Orochi Cloud (&quot;we&quot;, &quot;us&quot;, or &quot;our&quot;) is committed to protecting your privacy. This Privacy Policy explains how we collect, use, disclose, and safeguard your information when you use our managed database service.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">2. Information We Collect</h2>

            <h3 className="text-xl font-medium mt-4 mb-2">2.1 Account Information</h3>
            <p>When you create an account, we collect:</p>
            <ul className="list-disc pl-6 mt-2 space-y-1">
              <li>Email address</li>
              <li>Name</li>
              <li>Password (encrypted)</li>
              <li>Payment information (processed by our payment provider)</li>
            </ul>

            <h3 className="text-xl font-medium mt-4 mb-2">2.2 Usage Data</h3>
            <p>We automatically collect:</p>
            <ul className="list-disc pl-6 mt-2 space-y-1">
              <li>Database metrics (CPU, memory, storage usage)</li>
              <li>Query statistics (anonymized)</li>
              <li>Connection logs</li>
              <li>IP addresses and browser information</li>
            </ul>

            <h3 className="text-xl font-medium mt-4 mb-2">2.3 Your Data</h3>
            <p>
              The data you store in your databases remains your property. We access it only as necessary to provide the Service or when required by law.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">3. How We Use Your Information</h2>
            <p>We use collected information to:</p>
            <ul className="list-disc pl-6 mt-2 space-y-1">
              <li>Provide and maintain the Service</li>
              <li>Process transactions and send billing information</li>
              <li>Send service notifications and alerts</li>
              <li>Improve and optimize the Service</li>
              <li>Detect and prevent security threats</li>
              <li>Comply with legal obligations</li>
              <li>Send marketing communications (with your consent)</li>
            </ul>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">4. Data Retention</h2>
            <p>
              We retain your account information as long as your account is active. Database data is retained according to your backup settings. After account deletion, we retain data for 30 days before permanent deletion.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">5. Data Security</h2>
            <p>We implement industry-standard security measures including:</p>
            <ul className="list-disc pl-6 mt-2 space-y-1">
              <li>Encryption at rest and in transit (TLS 1.3)</li>
              <li>Regular security audits and penetration testing</li>
              <li>Access controls and authentication</li>
              <li>Network isolation and firewalls</li>
              <li>Continuous monitoring and threat detection</li>
            </ul>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">6. Data Sharing</h2>
            <p>We do not sell your personal information. We may share data with:</p>
            <ul className="list-disc pl-6 mt-2 space-y-1">
              <li>Cloud infrastructure providers (AWS, GCP, Azure) for hosting</li>
              <li>Payment processors for billing</li>
              <li>Analytics providers (anonymized data only)</li>
              <li>Legal authorities when required by law</li>
            </ul>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">7. Your Rights</h2>
            <p>You have the right to:</p>
            <ul className="list-disc pl-6 mt-2 space-y-1">
              <li>Access your personal data</li>
              <li>Correct inaccurate data</li>
              <li>Delete your account and data</li>
              <li>Export your data</li>
              <li>Opt out of marketing communications</li>
              <li>Restrict processing of your data</li>
            </ul>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">8. Cookies</h2>
            <p>
              We use essential cookies for authentication and session management. We use analytics cookies (with your consent) to improve the Service. You can control cookie preferences in your browser settings.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">9. International Data Transfers</h2>
            <p>
              Your data may be transferred to and processed in countries other than your own. We ensure appropriate safeguards are in place, including Standard Contractual Clauses for EU data transfers.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">10. Children&apos;s Privacy</h2>
            <p>
              The Service is not intended for users under 16 years of age. We do not knowingly collect information from children.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">11. Changes to This Policy</h2>
            <p>
              We may update this Privacy Policy from time to time. We will notify you of significant changes via email or through the Service.
            </p>
          </section>

          <section className="mt-8">
            <h2 className="text-2xl font-semibold mb-4">12. Contact Us</h2>
            <p>
              For questions about this Privacy Policy or to exercise your rights, contact us at:
            </p>
            <p className="mt-2">
              <strong>Email:</strong>{" "}
              <a href="mailto:privacy@orochi.dev" className="text-primary hover:underline">
                privacy@orochi.dev
              </a>
            </p>
            <p className="mt-2">
              <strong>Data Protection Officer:</strong>{" "}
              <a href="mailto:dpo@orochi.dev" className="text-primary hover:underline">
                dpo@orochi.dev
              </a>
            </p>
          </section>
        </div>
      </div>
    </div>
  );
}
