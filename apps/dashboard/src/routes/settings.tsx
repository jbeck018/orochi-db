import * as React from "react";
import { createFileRoute, useNavigate } from "@tanstack/react-router";
import {
  User,
  Lock,
  Bell,
  Shield,
  Trash2,
  Loader2,
  Save,
  Eye,
  EyeOff,
  AlertCircle,
  Check,
} from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Switch } from "@/components/ui/switch";
import { Separator } from "@/components/ui/separator";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Avatar, AvatarFallback, AvatarImage } from "@/components/ui/avatar";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { Skeleton } from "@/components/ui/skeleton";
import { useToast } from "@/hooks/use-toast";
import { userApi, notificationApi, twoFactorApi, type TwoFactorSetup, type TwoFactorStatus } from "@/lib/api";
import { getStoredUser, logout } from "@/lib/auth";
import type { User as UserType } from "@/types";

export const Route = createFileRoute("/settings")({
  component: SettingsPage,
});

function SettingsPage(): React.JSX.Element {
  const navigate = useNavigate();
  const { toast } = useToast();

  const [user, setUser] = React.useState<UserType | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);
  const [isSaving, setIsSaving] = React.useState(false);

  // Profile form
  const [name, setName] = React.useState("");
  const [email, setEmail] = React.useState("");

  // Password form
  const [currentPassword, setCurrentPassword] = React.useState("");
  const [newPassword, setNewPassword] = React.useState("");
  const [confirmPassword, setConfirmPassword] = React.useState("");
  const [showPasswords, setShowPasswords] = React.useState(false);

  // Delete account
  const [showDeleteDialog, setShowDeleteDialog] = React.useState(false);
  const [deletePassword, setDeletePassword] = React.useState("");

  // Notification preferences
  const [emailNotifications, setEmailNotifications] = React.useState(true);
  const [alertNotifications, setAlertNotifications] = React.useState(true);
  const [marketingEmails, setMarketingEmails] = React.useState(false);
  const [isLoadingNotifications, setIsLoadingNotifications] = React.useState(false);

  // 2FA state
  const [twoFactorStatus, setTwoFactorStatus] = React.useState<TwoFactorStatus | null>(null);
  const [twoFactorSetup, setTwoFactorSetup] = React.useState<TwoFactorSetup | null>(null);
  const [show2FADialog, setShow2FADialog] = React.useState(false);
  const [twoFactorCode, setTwoFactorCode] = React.useState("");
  const [backupCodes, setBackupCodes] = React.useState<string[]>([]);
  const [showBackupCodes, setShowBackupCodes] = React.useState(false);
  const [is2FALoading, setIs2FALoading] = React.useState(false);

  React.useEffect(() => {
    const loadData = async (): Promise<void> => {
      const storedUser = getStoredUser();
      if (storedUser) {
        setUser(storedUser);
        setName(storedUser.name);
        setEmail(storedUser.email);
      }

      // Load notification preferences
      try {
        const notifResponse = await notificationApi.get();
        setEmailNotifications(notifResponse.data.emailNotifications);
        setAlertNotifications(notifResponse.data.alertNotifications);
        setMarketingEmails(notifResponse.data.marketingEmails);
      } catch {
        // Use defaults if API fails
      }

      // Load 2FA status
      try {
        const tfaResponse = await twoFactorApi.getStatus();
        setTwoFactorStatus(tfaResponse.data);
      } catch {
        // Use defaults if API fails
      }

      setIsLoading(false);
    };

    loadData();
  }, []);

  const handleUpdateProfile = async (): Promise<void> => {
    setIsSaving(true);
    try {
      const response = await userApi.update({ name, email });
      setUser(response.data);
      toast({ title: "Profile updated" });
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Failed to update profile",
        variant: "destructive",
      });
    } finally {
      setIsSaving(false);
    }
  };

  const handleChangePassword = async (): Promise<void> => {
    if (newPassword !== confirmPassword) {
      toast({
        title: "Error",
        description: "Passwords do not match",
        variant: "destructive",
      });
      return;
    }

    setIsSaving(true);
    try {
      await userApi.changePassword(currentPassword, newPassword);
      toast({ title: "Password changed" });
      setCurrentPassword("");
      setNewPassword("");
      setConfirmPassword("");
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Failed to change password",
        variant: "destructive",
      });
    } finally {
      setIsSaving(false);
    }
  };

  const handleDeleteAccount = async (): Promise<void> => {
    setIsSaving(true);
    try {
      await userApi.deleteAccount(deletePassword);
      await logout();
      navigate({ to: "/login" });
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Failed to delete account",
        variant: "destructive",
      });
    } finally {
      setIsSaving(false);
      setShowDeleteDialog(false);
    }
  };

  const getInitials = (name: string): string => {
    return name
      .split(" ")
      .map((n) => n[0])
      .join("")
      .toUpperCase()
      .slice(0, 2);
  };

  const handleNotificationChange = async (
    type: "email" | "alert" | "marketing",
    value: boolean
  ): Promise<void> => {
    const newPrefs = {
      emailNotifications: type === "email" ? value : emailNotifications,
      alertNotifications: type === "alert" ? value : alertNotifications,
      marketingEmails: type === "marketing" ? value : marketingEmails,
    };

    // Update local state immediately
    if (type === "email") setEmailNotifications(value);
    if (type === "alert") setAlertNotifications(value);
    if (type === "marketing") setMarketingEmails(value);

    setIsLoadingNotifications(true);
    try {
      await notificationApi.update(newPrefs);
      toast({ title: "Notification preferences updated" });
    } catch (error) {
      // Revert on error
      if (type === "email") setEmailNotifications(!value);
      if (type === "alert") setAlertNotifications(!value);
      if (type === "marketing") setMarketingEmails(!value);
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Failed to update preferences",
        variant: "destructive",
      });
    } finally {
      setIsLoadingNotifications(false);
    }
  };

  const handleEnable2FA = async (): Promise<void> => {
    setIs2FALoading(true);
    try {
      const response = await twoFactorApi.enable();
      setTwoFactorSetup(response.data);
      setShow2FADialog(true);
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Failed to enable 2FA",
        variant: "destructive",
      });
    } finally {
      setIs2FALoading(false);
    }
  };

  const handleVerify2FA = async (): Promise<void> => {
    if (twoFactorCode.length !== 6) {
      toast({
        title: "Error",
        description: "Please enter a 6-digit code",
        variant: "destructive",
      });
      return;
    }

    setIs2FALoading(true);
    try {
      const response = await twoFactorApi.verify(twoFactorCode);
      setBackupCodes(response.data.backupCodes);
      setShowBackupCodes(true);
      setTwoFactorStatus({ enabled: true, enabledAt: new Date().toISOString() });
      setShow2FADialog(false);
      setTwoFactorSetup(null);
      setTwoFactorCode("");
      toast({ title: "Two-factor authentication enabled" });
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Invalid verification code",
        variant: "destructive",
      });
    } finally {
      setIs2FALoading(false);
    }
  };

  const handleDisable2FA = async (): Promise<void> => {
    if (twoFactorCode.length !== 6) {
      toast({
        title: "Error",
        description: "Please enter your 2FA code to disable",
        variant: "destructive",
      });
      return;
    }

    setIs2FALoading(true);
    try {
      await twoFactorApi.disable(twoFactorCode);
      setTwoFactorStatus({ enabled: false });
      setTwoFactorCode("");
      toast({ title: "Two-factor authentication disabled" });
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Failed to disable 2FA",
        variant: "destructive",
      });
    } finally {
      setIs2FALoading(false);
    }
  };

  if (isLoading) {
    return (
      <DashboardLayout>
        <div className="max-w-4xl mx-auto space-y-6">
          <Skeleton className="h-10 w-48" />
          <Skeleton className="h-[400px]" />
        </div>
      </DashboardLayout>
    );
  }

  return (
    <DashboardLayout>
      <div className="max-w-4xl mx-auto space-y-6">
        <div>
          <h1 className="text-3xl font-bold tracking-tight">Settings</h1>
          <p className="text-muted-foreground">
            Manage your account settings and preferences
          </p>
        </div>

        <Tabs defaultValue="profile" className="space-y-6">
          <TabsList>
            <TabsTrigger value="profile">
              <User className="mr-2 h-4 w-4" />
              Profile
            </TabsTrigger>
            <TabsTrigger value="security">
              <Shield className="mr-2 h-4 w-4" />
              Security
            </TabsTrigger>
            <TabsTrigger value="notifications">
              <Bell className="mr-2 h-4 w-4" />
              Notifications
            </TabsTrigger>
          </TabsList>

          {/* Profile Tab */}
          <TabsContent value="profile" className="space-y-6">
            <Card>
              <CardHeader>
                <CardTitle>Profile Information</CardTitle>
                <CardDescription>
                  Update your personal information
                </CardDescription>
              </CardHeader>
              <CardContent className="space-y-6">
                <div className="flex items-center gap-6">
                  <Avatar className="h-20 w-20">
                    <AvatarImage src={user?.avatar} alt={user?.name} />
                    <AvatarFallback className="text-2xl">
                      {user ? getInitials(user.name) : "?"}
                    </AvatarFallback>
                  </Avatar>
                  <div>
                    <Button variant="outline" size="sm">
                      Change Avatar
                    </Button>
                    <p className="text-xs text-muted-foreground mt-1">
                      JPG, GIF or PNG. Max size 2MB.
                    </p>
                  </div>
                </div>

                <Separator />

                <div className="grid gap-4">
                  <div className="space-y-2">
                    <Label htmlFor="name">Full Name</Label>
                    <Input
                      id="name"
                      value={name}
                      onChange={(e) => setName(e.target.value)}
                      disabled={isSaving}
                    />
                  </div>
                  <div className="space-y-2">
                    <Label htmlFor="email">Email Address</Label>
                    <Input
                      id="email"
                      type="email"
                      value={email}
                      onChange={(e) => setEmail(e.target.value)}
                      disabled={isSaving}
                    />
                    {user && !user.emailVerified && (
                      <p className="text-xs text-yellow-600">
                        Email not verified. Check your inbox for the verification link.
                      </p>
                    )}
                  </div>
                </div>

                <div className="flex justify-end">
                  <Button onClick={handleUpdateProfile} disabled={isSaving}>
                    {isSaving ? (
                      <>
                        <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                        Saving...
                      </>
                    ) : (
                      <>
                        <Save className="mr-2 h-4 w-4" />
                        Save Changes
                      </>
                    )}
                  </Button>
                </div>
              </CardContent>
            </Card>
          </TabsContent>

          {/* Security Tab */}
          <TabsContent value="security" className="space-y-6">
            <Card>
              <CardHeader>
                <CardTitle>Change Password</CardTitle>
                <CardDescription>
                  Update your password to keep your account secure
                </CardDescription>
              </CardHeader>
              <CardContent className="space-y-4">
                <div className="space-y-2">
                  <Label htmlFor="currentPassword">Current Password</Label>
                  <div className="relative">
                    <Input
                      id="currentPassword"
                      type={showPasswords ? "text" : "password"}
                      value={currentPassword}
                      onChange={(e) => setCurrentPassword(e.target.value)}
                      disabled={isSaving}
                    />
                    <Button
                      type="button"
                      variant="ghost"
                      size="icon"
                      className="absolute right-0 top-0 h-full px-3 hover:bg-transparent"
                      onClick={() => setShowPasswords(!showPasswords)}
                    >
                      {showPasswords ? (
                        <EyeOff className="h-4 w-4 text-muted-foreground" />
                      ) : (
                        <Eye className="h-4 w-4 text-muted-foreground" />
                      )}
                    </Button>
                  </div>
                </div>
                <div className="space-y-2">
                  <Label htmlFor="newPassword">New Password</Label>
                  <Input
                    id="newPassword"
                    type={showPasswords ? "text" : "password"}
                    value={newPassword}
                    onChange={(e) => setNewPassword(e.target.value)}
                    disabled={isSaving}
                  />
                </div>
                <div className="space-y-2">
                  <Label htmlFor="confirmPassword">Confirm New Password</Label>
                  <Input
                    id="confirmPassword"
                    type={showPasswords ? "text" : "password"}
                    value={confirmPassword}
                    onChange={(e) => setConfirmPassword(e.target.value)}
                    disabled={isSaving}
                  />
                  {confirmPassword && newPassword !== confirmPassword && (
                    <p className="text-xs text-destructive">
                      Passwords do not match
                    </p>
                  )}
                </div>
                <div className="flex justify-end">
                  <Button
                    onClick={handleChangePassword}
                    disabled={
                      isSaving ||
                      !currentPassword ||
                      !newPassword ||
                      newPassword !== confirmPassword
                    }
                  >
                    {isSaving ? (
                      <>
                        <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                        Updating...
                      </>
                    ) : (
                      <>
                        <Lock className="mr-2 h-4 w-4" />
                        Update Password
                      </>
                    )}
                  </Button>
                </div>
              </CardContent>
            </Card>

            <Card>
              <CardHeader>
                <CardTitle>Two-Factor Authentication</CardTitle>
                <CardDescription>
                  Add an extra layer of security to your account
                </CardDescription>
              </CardHeader>
              <CardContent className="space-y-4">
                <div className="flex items-center justify-between">
                  <div className="flex items-center gap-3">
                    <div className="flex h-10 w-10 items-center justify-center rounded-lg bg-primary/10">
                      <Shield className="h-5 w-5 text-primary" />
                    </div>
                    <div>
                      <p className="font-medium">Authenticator App</p>
                      <p className="text-sm text-muted-foreground">
                        {twoFactorStatus?.enabled
                          ? "Two-factor authentication is enabled"
                          : "Use an authenticator app to generate one-time codes"
                        }
                      </p>
                    </div>
                  </div>
                  {twoFactorStatus?.enabled ? (
                    <div className="flex items-center gap-2">
                      <Check className="h-4 w-4 text-green-600" />
                      <span className="text-sm text-green-600">Enabled</span>
                    </div>
                  ) : (
                    <Button
                      variant="outline"
                      onClick={handleEnable2FA}
                      disabled={is2FALoading}
                    >
                      {is2FALoading ? (
                        <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                      ) : null}
                      Enable
                    </Button>
                  )}
                </div>
                {twoFactorStatus?.enabled && (
                  <div className="pt-4 border-t">
                    <p className="text-sm font-medium mb-2">Disable 2FA</p>
                    <p className="text-sm text-muted-foreground mb-3">
                      Enter your current 2FA code to disable two-factor authentication.
                    </p>
                    <div className="flex gap-2">
                      <Input
                        placeholder="Enter 6-digit code"
                        value={twoFactorCode}
                        onChange={(e) => setTwoFactorCode(e.target.value.replace(/\D/g, "").slice(0, 6))}
                        maxLength={6}
                        className="w-40"
                      />
                      <Button
                        variant="destructive"
                        onClick={handleDisable2FA}
                        disabled={is2FALoading || twoFactorCode.length !== 6}
                      >
                        {is2FALoading ? (
                          <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                        ) : null}
                        Disable 2FA
                      </Button>
                    </div>
                  </div>
                )}
              </CardContent>
            </Card>

            <Card className="border-destructive">
              <CardHeader>
                <CardTitle className="text-destructive">Danger Zone</CardTitle>
                <CardDescription>
                  Irreversible and destructive actions
                </CardDescription>
              </CardHeader>
              <CardContent>
                <div className="flex items-center justify-between">
                  <div>
                    <p className="font-medium">Delete Account</p>
                    <p className="text-sm text-muted-foreground">
                      Permanently delete your account and all associated data
                    </p>
                  </div>
                  <Button
                    variant="destructive"
                    onClick={() => setShowDeleteDialog(true)}
                  >
                    <Trash2 className="mr-2 h-4 w-4" />
                    Delete Account
                  </Button>
                </div>
              </CardContent>
            </Card>
          </TabsContent>

          {/* Notifications Tab */}
          <TabsContent value="notifications" className="space-y-6">
            <Card>
              <CardHeader>
                <CardTitle>Email Notifications</CardTitle>
                <CardDescription>
                  Choose what emails you want to receive
                </CardDescription>
              </CardHeader>
              <CardContent className="space-y-6">
                <div className="flex items-center justify-between">
                  <div className="space-y-0.5">
                    <Label>Cluster Notifications</Label>
                    <p className="text-sm text-muted-foreground">
                      Get notified about cluster status changes
                    </p>
                  </div>
                  <Switch
                    checked={emailNotifications}
                    onCheckedChange={(checked) => handleNotificationChange("email", checked)}
                    disabled={isLoadingNotifications}
                  />
                </div>
                <Separator />
                <div className="flex items-center justify-between">
                  <div className="space-y-0.5">
                    <Label>Alert Notifications</Label>
                    <p className="text-sm text-muted-foreground">
                      Receive alerts for high resource usage and errors
                    </p>
                  </div>
                  <Switch
                    checked={alertNotifications}
                    onCheckedChange={(checked) => handleNotificationChange("alert", checked)}
                    disabled={isLoadingNotifications}
                  />
                </div>
                <Separator />
                <div className="flex items-center justify-between">
                  <div className="space-y-0.5">
                    <Label>Marketing Emails</Label>
                    <p className="text-sm text-muted-foreground">
                      Receive news, updates, and promotional content
                    </p>
                  </div>
                  <Switch
                    checked={marketingEmails}
                    onCheckedChange={(checked) => handleNotificationChange("marketing", checked)}
                    disabled={isLoadingNotifications}
                  />
                </div>
              </CardContent>
            </Card>
          </TabsContent>
        </Tabs>

        {/* 2FA Setup Dialog */}
        <Dialog open={show2FADialog} onOpenChange={setShow2FADialog}>
          <DialogContent className="max-w-md">
            <DialogHeader>
              <DialogTitle>Set up Two-Factor Authentication</DialogTitle>
              <DialogDescription>
                Scan the QR code with your authenticator app, then enter the
                verification code.
              </DialogDescription>
            </DialogHeader>
            {twoFactorSetup && (
              <div className="space-y-4 py-4">
                <div className="flex justify-center">
                  <div className="p-4 bg-white rounded-lg">
                    <img
                      src={twoFactorSetup.qrCodeUrl}
                      alt="2FA QR Code"
                      className="w-48 h-48"
                    />
                  </div>
                </div>
                <div className="text-center">
                  <p className="text-sm text-muted-foreground mb-1">
                    Or enter this code manually:
                  </p>
                  <code className="text-sm bg-muted px-2 py-1 rounded font-mono">
                    {twoFactorSetup.manualEntryKey}
                  </code>
                </div>
                <div className="space-y-2">
                  <Label htmlFor="verifyCode">Verification Code</Label>
                  <Input
                    id="verifyCode"
                    placeholder="Enter 6-digit code"
                    value={twoFactorCode}
                    onChange={(e) => setTwoFactorCode(e.target.value.replace(/\D/g, "").slice(0, 6))}
                    maxLength={6}
                    className="text-center text-lg tracking-widest"
                  />
                </div>
              </div>
            )}
            <DialogFooter>
              <Button
                variant="outline"
                onClick={() => {
                  setShow2FADialog(false);
                  setTwoFactorSetup(null);
                  setTwoFactorCode("");
                }}
              >
                Cancel
              </Button>
              <Button
                onClick={handleVerify2FA}
                disabled={is2FALoading || twoFactorCode.length !== 6}
              >
                {is2FALoading ? (
                  <>
                    <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                    Verifying...
                  </>
                ) : (
                  "Verify & Enable"
                )}
              </Button>
            </DialogFooter>
          </DialogContent>
        </Dialog>

        {/* Backup Codes Dialog */}
        <Dialog open={showBackupCodes} onOpenChange={setShowBackupCodes}>
          <DialogContent className="max-w-md">
            <DialogHeader>
              <DialogTitle>Save Your Backup Codes</DialogTitle>
              <DialogDescription>
                Store these codes in a safe place. You can use them to access
                your account if you lose your authenticator device.
              </DialogDescription>
            </DialogHeader>
            <div className="py-4">
              <Alert>
                <AlertCircle className="h-4 w-4" />
                <AlertDescription>
                  Each code can only be used once. Keep them secure!
                </AlertDescription>
              </Alert>
              <div className="mt-4 grid grid-cols-2 gap-2">
                {backupCodes.map((code, index) => (
                  <code
                    key={index}
                    className="bg-muted px-3 py-2 rounded text-center font-mono text-sm"
                  >
                    {code}
                  </code>
                ))}
              </div>
            </div>
            <DialogFooter>
              <Button
                variant="outline"
                onClick={() => {
                  navigator.clipboard.writeText(backupCodes.join("\n"));
                  toast({ title: "Backup codes copied to clipboard" });
                }}
              >
                Copy Codes
              </Button>
              <Button onClick={() => setShowBackupCodes(false)}>
                I've Saved My Codes
              </Button>
            </DialogFooter>
          </DialogContent>
        </Dialog>

        {/* Delete Account Dialog */}
        <Dialog open={showDeleteDialog} onOpenChange={setShowDeleteDialog}>
          <DialogContent>
            <DialogHeader>
              <DialogTitle>Delete Account</DialogTitle>
              <DialogDescription>
                This action cannot be undone. This will permanently delete your
                account, all clusters, and all associated data.
              </DialogDescription>
            </DialogHeader>
            <div className="space-y-4 py-4">
              <Alert variant="destructive">
                <AlertCircle className="h-4 w-4" />
                <AlertDescription>
                  All your clusters will be terminated and data will be lost.
                </AlertDescription>
              </Alert>
              <div className="space-y-2">
                <Label htmlFor="deletePassword">
                  Enter your password to confirm
                </Label>
                <Input
                  id="deletePassword"
                  type="password"
                  value={deletePassword}
                  onChange={(e) => setDeletePassword(e.target.value)}
                  placeholder="Enter your password"
                />
              </div>
            </div>
            <DialogFooter>
              <Button
                variant="outline"
                onClick={() => setShowDeleteDialog(false)}
              >
                Cancel
              </Button>
              <Button
                variant="destructive"
                onClick={handleDeleteAccount}
                disabled={!deletePassword || isSaving}
              >
                {isSaving ? (
                  <>
                    <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                    Deleting...
                  </>
                ) : (
                  "Delete Account"
                )}
              </Button>
            </DialogFooter>
          </DialogContent>
        </Dialog>
      </div>
    </DashboardLayout>
  );
}
