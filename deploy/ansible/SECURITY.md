# Security Guidelines for Deployment Configuration

## Protecting Sensitive Infrastructure Data

This deployment contains sensitive information (server IPs, SSH keys) that should NOT be committed to public repositories.

## Quick Setup

```bash
# First time setup
./setup-inventory.sh

# Choose option 1 and edit the created inventory.yml with your actual IPs
# The inventory.yml file is gitignored and won't be committed
```

## Configuration Methods

### Method 1: Template File (Recommended)
- **Setup**: Copy `inventory.yml.example` to `inventory.yml`
- **Security**: `inventory.yml` is gitignored
- **Pros**: Simple, no extra tools needed
- **Cons**: Must manually manage file

### Method 2: Environment Variables
```bash
# Set your node IPs
export NODE1_IP=x.x.x.x
export NODE2_IP=x.x.x.x
# ... etc

# Generate inventory
./setup-inventory.sh  # Choose option 2
```

### Method 3: Ansible Vault (For Teams)
```bash
# Create encrypted inventory
./setup-inventory.sh  # Choose option 3

# Deploy with password
ansible-playbook -i inventory.yml deploy-simple.yml --ask-vault-pass
```

### Method 4: .env File
```bash
# Copy and edit .env file
cp .env.example .env
nano .env

# Generate inventory from .env
./setup-inventory.sh  # Choose option 4
```

## Files to NEVER Commit

- `inventory.yml` - Contains actual server IPs
- `.env` - Contains environment secrets
- `*.vault` - Encrypted files with passwords
- Any file with actual IPs, passwords, or API keys

## Files Safe to Commit

- `inventory.yml.example` - Template with dummy data
- `.env.example` - Template with dummy data
- `SECURITY.md` - This file
- `setup-inventory.sh` - Setup script

## Best Practices

1. **Use SSH Keys**: Never put passwords in files
   ```bash
   ansible_ssh_private_key_file: ~/.ssh/deploy_key
   ```

2. **Separate Repos**: Consider keeping deployment configs in a private repo
   ```bash
   # Public repo: source code
   github.com/yourname/unicity

   # Private repo: deployment
   github.com/yourname/unicity-deploy  # Private
   ```

3. **Use CI/CD Secrets**: For automated deployments
   - GitHub Actions Secrets
   - GitLab CI Variables
   - Jenkins Credentials

4. **Rotate Regularly**: Change IPs/keys periodically

5. **Audit Access**: Track who has access to infrastructure

## Checking for Leaks

Before committing:
```bash
# Check what will be committed
git status
git diff --staged

# Search for IPs (example pattern)
git diff --staged | grep -E '\b[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\b'

# Make sure inventory.yml is NOT in the list
git ls-files | grep inventory.yml  # Should return nothing
```

## If You Accidentally Commit Secrets

1. **Immediately**: Rotate all exposed credentials
2. **Remove from history**:
   ```bash
   git filter-branch --force --index-filter \
     "git rm --cached --ignore-unmatch inventory.yml" \
     --prune-empty --tag-name-filter cat -- --all
   ```
3. **Force push**: `git push --force --all`
4. **Notify**: Tell your team to re-clone

## Questions?

- Keep infrastructure details private
- Use templates for public repos
- Encrypt or externalize sensitive data