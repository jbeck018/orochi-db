"use client";

import * as React from "react";
import { Loader2, Mail, Copy, Check } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { useCreateInvite } from "@/hooks/api";
import type { OrganizationRole, OrganizationInvite } from "@/types";

interface InviteDialogProps {
  organizationId: string;
  organizationName: string;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

export function InviteDialog({
  organizationId,
  organizationName,
  open,
  onOpenChange,
}: InviteDialogProps) {
  const [email, setEmail] = React.useState("");
  const [role, setRole] = React.useState<OrganizationRole>("member");
  const [createdInvite, setCreatedInvite] = React.useState<OrganizationInvite | null>(null);
  const [copied, setCopied] = React.useState(false);

  const createInviteMutation = useCreateInvite();

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();

    try {
      const invite = await createInviteMutation.mutateAsync({
        organizationId,
        data: { email, role },
      });
      setCreatedInvite(invite);
    } catch (err) {
      // Error will be shown from mutation
    }
  };

  const handleClose = () => {
    setEmail("");
    setRole("member");
    setCreatedInvite(null);
    setCopied(false);
    createInviteMutation.reset();
    onOpenChange(false);
  };

  const inviteUrl = createdInvite
    ? `${window.location.origin}/invites/${createdInvite.token}`
    : "";

  const handleCopy = async () => {
    await navigator.clipboard.writeText(inviteUrl);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  // Success state - show invite link
  if (createdInvite) {
    return (
      <Dialog open={open} onOpenChange={handleClose}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>Invitation Sent!</DialogTitle>
            <DialogDescription>
              We've sent an invitation to <strong>{createdInvite.email}</strong>. You can also share
              the link below directly.
            </DialogDescription>
          </DialogHeader>

          <div className="space-y-4 py-4">
            <div className="space-y-2">
              <Label>Invitation Link</Label>
              <div className="flex gap-2">
                <Input
                  value={inviteUrl}
                  readOnly
                  className="font-mono text-sm"
                />
                <Button
                  type="button"
                  variant="outline"
                  size="icon"
                  onClick={handleCopy}
                >
                  {copied ? (
                    <Check className="h-4 w-4 text-green-600" />
                  ) : (
                    <Copy className="h-4 w-4" />
                  )}
                </Button>
              </div>
              <p className="text-xs text-muted-foreground">
                This link expires in 24 hours.
              </p>
            </div>
          </div>

          <DialogFooter>
            <Button onClick={handleClose}>Done</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    );
  }

  return (
    <Dialog open={open} onOpenChange={handleClose}>
      <DialogContent>
        <form onSubmit={handleSubmit}>
          <DialogHeader>
            <DialogTitle>Invite Team Member</DialogTitle>
            <DialogDescription>
              Invite someone to join {organizationName}. They'll receive an email
              with instructions to join.
            </DialogDescription>
          </DialogHeader>

          <div className="space-y-4 py-4">
            {createInviteMutation.isError && (
              <Alert variant="destructive">
                <AlertDescription>
                  {createInviteMutation.error instanceof Error
                    ? createInviteMutation.error.message
                    : "Failed to send invitation"}
                </AlertDescription>
              </Alert>
            )}

            <div className="space-y-2">
              <Label htmlFor="email">Email Address</Label>
              <Input
                id="email"
                type="email"
                placeholder="colleague@company.com"
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                required
                disabled={createInviteMutation.isPending}
              />
            </div>

            <div className="space-y-2">
              <Label htmlFor="role">Role</Label>
              <Select
                value={role}
                onValueChange={(value) => setRole(value as OrganizationRole)}
                disabled={createInviteMutation.isPending}
              >
                <SelectTrigger id="role">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="viewer">
                    <div className="flex flex-col">
                      <span>Viewer</span>
                      <span className="text-xs text-muted-foreground">
                        Can view clusters and data
                      </span>
                    </div>
                  </SelectItem>
                  <SelectItem value="member">
                    <div className="flex flex-col">
                      <span>Member</span>
                      <span className="text-xs text-muted-foreground">
                        Can manage clusters
                      </span>
                    </div>
                  </SelectItem>
                  <SelectItem value="admin">
                    <div className="flex flex-col">
                      <span>Admin</span>
                      <span className="text-xs text-muted-foreground">
                        Can manage organization settings
                      </span>
                    </div>
                  </SelectItem>
                </SelectContent>
              </Select>
            </div>
          </div>

          <DialogFooter>
            <Button
              type="button"
              variant="outline"
              onClick={handleClose}
              disabled={createInviteMutation.isPending}
            >
              Cancel
            </Button>
            <Button type="submit" disabled={createInviteMutation.isPending}>
              {createInviteMutation.isPending ? (
                <>
                  <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                  Sending...
                </>
              ) : (
                <>
                  <Mail className="mr-2 h-4 w-4" />
                  Send Invitation
                </>
              )}
            </Button>
          </DialogFooter>
        </form>
      </DialogContent>
    </Dialog>
  );
}
