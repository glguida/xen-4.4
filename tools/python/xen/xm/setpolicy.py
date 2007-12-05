#============================================================================
# This library is free software; you can redistribute it and/or
# modify it under the terms of version 2.1 of the GNU Lesser General Public
# License as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#============================================================================
# Copyright (C) 2007 International Business Machines Corp.
# Author: Stefan Berger <stefanb@us.ibm.com>
#============================================================================

"""Get the managed policy of the system.
"""

import os
import sys
import base64
import struct
import xen.util.xsm.xsm as security
from xen.util import xsconstants
from xen.util.acmpolicy import ACMPolicy, \
   ACM_EVTCHN_SHARING_VIOLATION,\
   ACM_GNTTAB_SHARING_VIOLATION, \
   ACM_DOMAIN_LOOKUP,   \
   ACM_CHWALL_CONFLICT, \
   ACM_SSIDREF_IN_USE
from xen.xm.opts import OptionError
from xen.util.xsm.acm.acm import policy_dir_prefix
from xen.xm import main as xm_main
from xen.xm.getpolicy import getpolicy
from xen.xm.main import server

def help():
    return """
    Usage: xm setpolicy <policytype> <policyname>

    Set the policy managed by xend.

    The only policytype that is currently supported is 'ACM'.

    The filename of the policy is the policy name plus the suffic
    '-security_policy.xml'. The location of the policy file is either
    the the current directory or '/etc/xen/acm-security/policies'.

    """

def build_hv_error_message(errors):
    """
       Build a message from the error codes return by the hypervisor.
    """
    txt = "Hypervisor reported errors:"
    i = 0
    while i + 7 < len(errors):
        code, data = struct.unpack("!ii", errors[i:i+8])
        err_msgs  = {
            ACM_EVTCHN_SHARING_VIOLATION : \
                    ["event channel sharing violation between domains",2],
            ACM_GNTTAB_SHARING_VIOLATION : \
                    ["grant table sharing violation between domains",2],
            ACM_DOMAIN_LOOKUP : \
                    ["domain lookup",1],
            ACM_CHWALL_CONFLICT : \
                    ["Chinese Wall conflict between domains",2],
            ACM_SSIDREF_IN_USE : \
                    ["A domain used SSIDREF",1],
        }
        num = err_msgs[code][1]
        if num == 1:
            txt += "%s %d" % (err_msgs[code][0], data)
        else:
            txt += "%s %d and %d" % (err_msgs[code][0],
                                     data >> 16 , data & 0xffff)
        i += 8
    return txt


def setpolicy(policytype, policy_name, flags, overwrite):

    if policytype.upper() == xsconstants.ACM_POLICY_ID:
        xs_type = xsconstants.XS_POLICY_ACM

        for prefix in [ './', policy_dir_prefix+"/" ]:
            policy_file = prefix + "/".join(policy_name.split(".")) + \
                          "-security_policy.xml"

            if os.path.exists(policy_file):
                break

        try:
            f = open(policy_file,"r")
            xml = f.read()
            f.close()
        except:
            raise OptionError("Could not read policy file from current"
                              " directory or '%s'." % policy_dir_prefix)

        if xm_main.serverType == xm_main.SERVER_XEN_API:

            try:
                policystate = server.xenapi.XSPolicy.set_xspolicy(xs_type,
                                                                  xml,
                                                                  flags,
                                                                  overwrite)
            except Exception, e:
                raise security.XSMError("An error occurred setting the "
                                        "policy: %s" % str(e))
            xserr = int(policystate['xserr'])
            if xserr != xsconstants.XSERR_SUCCESS:
                txt = "An error occurred trying to set the policy: %s." % \
                      xsconstants.xserr2string(abs(xserr))
                errors = policystate['errors']
                if len(errors) > 0:
                    txt += " " + build_hv_error_message(base64.b64decode(errors))
                raise security.XSMError(txt)
            else:
                print "Successfully set the new policy."
                getpolicy(False)
        else:
            # Non-Xen-API call.

            rc, errors = server.xend.security.set_policy(xs_type,
                                                         xml,
                                                         flags,
                                                         overwrite)
            if rc != xsconstants.XSERR_SUCCESS:
                txt = "An error occurred trying to set the policy: %s." % \
                      xsconstants.xserr2string(abs(rc))
                if len(errors) > 0:
                    txt += " " + build_hv_error_message(
                                       base64.b64decode(errors))
                raise security.XSMError(txt)
            else:
                print "Successfully set the new policy."
                getpolicy(False)
    else:
        raise OptionError("Unsupported policytype '%s'." % policytype)


def main(argv):
    if len(argv) < 3:
       raise OptionError("Need at least 3 arguments.")

    if "-?" in argv:
        help()
        return

    policytype  = argv[1]
    policy_name = argv[2]

    flags = xsconstants.XS_INST_LOAD | xsconstants.XS_INST_BOOT
    overwrite = True

    setpolicy(policytype, policy_name, flags, overwrite)

if __name__ == '__main__':
    try:
        main(sys.argv)
    except Exception, e:
        sys.stderr.write('Error: %s\n' % str(e))
        sys.exit(-1)
