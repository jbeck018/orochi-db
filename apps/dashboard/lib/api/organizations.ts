import { getAuthHeader } from "@/lib/auth";
import type { Organization, OrganizationMember, OrganizationRole, CreateOrganizationRequest } from "@/types";

const API_URL = import.meta.env.VITE_API_URL ?? "http://localhost:8080";

// List user's organizations
export async function listOrganizations(): Promise<Organization[]> {
  const response = await fetch(`${API_URL}/api/v1/organizations`, {
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to fetch organizations" }));
    throw new Error(error.message ?? "Failed to fetch organizations");
  }

  const data = await response.json();
  return (data.organizations ?? []).map(mapOrganizationFromApi);
}

// Get organization by ID
export async function getOrganization(id: string): Promise<Organization> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${id}`, {
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Organization not found" }));
    throw new Error(error.message ?? "Organization not found");
  }

  const data = await response.json();
  return mapOrganizationFromApi(data);
}

// Create organization
export async function createOrganization(request: CreateOrganizationRequest): Promise<Organization> {
  const response = await fetch(`${API_URL}/api/v1/organizations`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
    body: JSON.stringify(request),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to create organization" }));
    throw new Error(error.message ?? "Failed to create organization");
  }

  const data = await response.json();
  return mapOrganizationFromApi(data);
}

// Update organization
export async function updateOrganization(id: string, request: Partial<CreateOrganizationRequest>): Promise<Organization> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${id}`, {
    method: "PATCH",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
    body: JSON.stringify(request),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to update organization" }));
    throw new Error(error.message ?? "Failed to update organization");
  }

  const data = await response.json();
  return mapOrganizationFromApi(data);
}

// Delete organization
export async function deleteOrganization(id: string): Promise<void> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${id}`, {
    method: "DELETE",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to delete organization" }));
    throw new Error(error.message ?? "Failed to delete organization");
  }
}

// Get organization members
export async function getOrganizationMembers(organizationId: string): Promise<OrganizationMember[]> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${organizationId}/members`, {
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to fetch members" }));
    throw new Error(error.message ?? "Failed to fetch members");
  }

  const data = await response.json();
  return (data.members ?? []).map(mapMemberFromApi);
}

// Remove a member from organization
export async function removeMember(organizationId: string, memberId: string): Promise<void> {
  const response = await fetch(`${API_URL}/api/v1/organizations/${organizationId}/members/${memberId}`, {
    method: "DELETE",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ message: "Failed to remove member" }));
    throw new Error(error.message ?? "Failed to remove member");
  }
}

// Helper to map API response to frontend format
function mapOrganizationFromApi(data: Record<string, unknown>): Organization {
  return {
    id: data.id as string,
    name: data.name as string,
    slug: data.slug as string,
    createdAt: data.created_at as string,
    updatedAt: data.updated_at as string,
  };
}

function mapMemberFromApi(data: Record<string, unknown>): OrganizationMember {
  return {
    id: data.id as string,
    userId: data.user_id as string,
    organizationId: data.organization_id as string,
    role: data.role as OrganizationRole,
    email: data.email as string,
    name: data.name as string,
    joinedAt: data.joined_at as string,
  };
}
