import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { organizationApi, inviteApi } from "@/lib/api";
import type { CreateOrganizationRequest, CreateInviteRequest } from "@/types";

// Query keys
export const organizationKeys = {
  all: ["organizations"] as const,
  list: () => [...organizationKeys.all, "list"] as const,
  detail: (id: string) => [...organizationKeys.all, "detail", id] as const,
  members: (id: string) => [...organizationKeys.all, "members", id] as const,
  invites: (id: string) => [...organizationKeys.all, "invites", id] as const,
};

export const inviteKeys = {
  all: ["invites"] as const,
  mine: () => [...inviteKeys.all, "mine"] as const,
  byToken: (token: string) => [...inviteKeys.all, "token", token] as const,
};

// Organization queries
export function useOrganizations() {
  return useQuery({
    queryKey: organizationKeys.list(),
    queryFn: () => organizationApi.list(),
  });
}

export function useOrganization(id: string) {
  return useQuery({
    queryKey: organizationKeys.detail(id),
    queryFn: () => organizationApi.get(id),
    enabled: Boolean(id),
  });
}

export function useOrganizationMembers(organizationId: string) {
  return useQuery({
    queryKey: organizationKeys.members(organizationId),
    queryFn: () => organizationApi.getMembers(organizationId),
    enabled: Boolean(organizationId),
  });
}

// Organization mutations
export function useCreateOrganization() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (data: CreateOrganizationRequest) => organizationApi.create(data),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: organizationKeys.list() });
    },
  });
}

export function useUpdateOrganization() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: ({ id, data }: { id: string; data: Partial<CreateOrganizationRequest> }) =>
      organizationApi.update(id, data),
    onSuccess: (_, { id }) => {
      queryClient.invalidateQueries({ queryKey: organizationKeys.detail(id) });
      queryClient.invalidateQueries({ queryKey: organizationKeys.list() });
    },
  });
}

export function useDeleteOrganization() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (id: string) => organizationApi.delete(id),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: organizationKeys.list() });
    },
  });
}

export function useRemoveOrganizationMember() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: ({ organizationId, memberId }: { organizationId: string; memberId: string }) =>
      organizationApi.removeMember(organizationId, memberId),
    onSuccess: (_, { organizationId }) => {
      queryClient.invalidateQueries({ queryKey: organizationKeys.members(organizationId) });
    },
  });
}

// Invite queries
export function useMyInvites() {
  return useQuery({
    queryKey: inviteKeys.mine(),
    queryFn: () => inviteApi.listMine(),
  });
}

export function useInviteByToken(token: string) {
  return useQuery({
    queryKey: inviteKeys.byToken(token),
    queryFn: () => inviteApi.getByToken(token),
    enabled: Boolean(token),
    retry: false,
  });
}

export function useOrganizationInvites(organizationId: string) {
  return useQuery({
    queryKey: organizationKeys.invites(organizationId),
    queryFn: () => inviteApi.listForOrganization(organizationId),
    enabled: Boolean(organizationId),
  });
}

// Invite mutations
export function useAcceptInvite() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (token: string) => inviteApi.accept(token),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: inviteKeys.mine() });
      queryClient.invalidateQueries({ queryKey: organizationKeys.list() });
    },
  });
}

export function useCreateInvite() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: ({ organizationId, data }: { organizationId: string; data: CreateInviteRequest }) =>
      inviteApi.create(organizationId, data),
    onSuccess: (_, { organizationId }) => {
      queryClient.invalidateQueries({ queryKey: organizationKeys.invites(organizationId) });
    },
  });
}

export function useRevokeInvite() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: ({ organizationId, inviteId }: { organizationId: string; inviteId: string }) =>
      inviteApi.revoke(organizationId, inviteId),
    onSuccess: (_, { organizationId }) => {
      queryClient.invalidateQueries({ queryKey: organizationKeys.invites(organizationId) });
    },
  });
}

export function useResendInvite() {
  return useMutation({
    mutationFn: ({ organizationId, inviteId }: { organizationId: string; inviteId: string }) =>
      inviteApi.resend(organizationId, inviteId),
  });
}
