import { getAuthHeader } from "@/lib/auth";
import type { OrganizationInvite, CreateInviteRequest, OrganizationRole } from "@/types";

const API_URL = import.meta.env.VITE_API_URL ?? "http://localhost:8080";

// Get invite by token (public - no auth required)
export async function getInviteByToken(token: string): Promise<OrganizationInvite> {
  const response = await fetch(`${API_URL}/api/v1/invites/${token}`);

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Invite not found" }));
    throw new Error(error.message ?? "Invite not found");
  }

  const data = await response.json();
  return mapInviteFromApi(data);
}

// Accept an invite (auth required)
export async function acceptInvite(token: string): Promise<void> {
  const response = await fetch(`${API_URL}/api/v1/invites/${token}/accept`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to accept invite" }));
    throw new Error(error.message ?? "Failed to accept invite");
  }
}

// List my pending invites (auth required)
export async function listMyInvites(): Promise<OrganizationInvite[]> {
  const response = await fetch(`${API_URL}/api/v1/invites/me`, {
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to fetch invites" }));
    throw new Error(error.message ?? "Failed to fetch invites");
  }

  const data = await response.json();
  return (data.invites ?? []).map(mapInviteFromApi);
}

// List invites for an organization (auth required, admin only)
export async function listOrganizationInvites(organizationId: string): Promise<OrganizationInvite[]> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${organizationId}/invites`, {
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to fetch invites" }));
    throw new Error(error.message ?? "Failed to fetch invites");
  }

  const data = await response.json();
  return (data.invites ?? []).map(mapInviteFromApi);
}

// Create an invite (auth required, admin only)
export async function createInvite(organizationId: string, request: CreateInviteRequest): Promise<OrganizationInvite> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${organizationId}/invites`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
    body: JSON.stringify(request),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to create invite" }));
    throw new Error(error.message ?? "Failed to create invite");
  }

  const data = await response.json();
  return mapInviteFromApi(data);
}

// Revoke an invite (auth required, admin only)
export async function revokeInvite(organizationId: string, inviteId: string): Promise<void> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${organizationId}/invites/${inviteId}`, {
    method: "DELETE",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to revoke invite" }));
    throw new Error(error.message ?? "Failed to revoke invite");
  }
}

// Resend an invite (auth required, admin only)
export async function resendInvite(organizationId: string, inviteId: string): Promise<void> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${organizationId}/invites/${inviteId}/resend`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to resend invite" }));
    throw new Error(error.message ?? "Failed to resend invite");
  }
}

// Helper to map API response to frontend format
function mapInviteFromApi(data: Record<string, unknown>): OrganizationInvite {
  return {
    id: data.id as string,
    organizationId: data.organization_id as string,
    organizationName: data.organization_name as string,
    email: data.email as string,
    role: data.role as OrganizationRole,
    invitedByName: data.invited_by_name as string,
    token: data.token as string,
    expiresAt: data.expires_at as string,
    acceptedAt: data.accepted_at as string | undefined,
    createdAt: data.created_at as string,
  };
}
