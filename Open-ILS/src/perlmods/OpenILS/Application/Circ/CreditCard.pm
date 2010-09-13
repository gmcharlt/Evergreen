# --------------------------------------------------------------------
# Copyright (C) 2008 Niles Ingalls 
# Niles Ingalls <nilesi@zionsville.lib.in.us>
# Bill Erickson <erickson@esilibrary.com>
# Joe Atzberger <jatzberger@esilibrary.com>
# Lebbeous Fogle-Weekley <lebbeous@esilibrary.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# --------------------------------------------------------------------
package OpenILS::Application::Circ::CreditCard;
use base qw/OpenSRF::Application/;
use strict; use warnings;

use Business::CreditCard;
use Business::OnlinePayment;
use Locale::Country;

use OpenILS::Event;
use OpenSRF::Utils::Logger qw/:logger/;
use OpenILS::Utils::CStoreEditor qw/:funcs/;
use OpenILS::Application::AppUtils;
my $U = "OpenILS::Application::AppUtils";

use constant CREDIT_NS => "credit";

# Given the argshash from process_payment(), this helper function just finds
# a function in the current namespace named "bop_args_{processor}" and calls
# it with $argshash as an argument, returning the result, or returning an
# empty hash if it can't find such a function.
sub get_bop_args_filler {
    no strict 'refs';

    my $argshash = shift;
    my $funcname = "bop_args_" . $argshash->{processor};
    return &{$funcname}($argshash) if defined &{$funcname};
    return ();
}

# Provide default arguments for calls using the AuthorizeNet processor
sub bop_args_AuthorizeNet {
    my $argshash = shift;
    if ($argshash->{server}) {
        return (
            # One might provide "test.authorize.net" here.
            Server => $argshash->{server},
        );
    }
    else {
        return ();
    }
}

# Provide default arguments for calls using the PayPal processor
sub bop_args_PayPal {
    my $argshash = shift;
    return (
        Username => $argshash->{login},
        Password => $argshash->{password},
        Signature => $argshash->{signature}
    );
}

sub get_processor_settings {
    my $org_unit = shift;
    my $processor = lc shift;

    +{ map { ($_ =>
        $U->ou_ancestor_setting_value(
            $org_unit, CREDIT_NS . ".processor.${processor}.${_}"
        )) } qw/enabled login password signature server testmode/
    };
}

#    signature => {
#        desc   => 'Process a payment via a supported processor (AuthorizeNet, Paypal)',
#        params => [
#            { desc => q/Hash of arguments with these keys:
#                patron_id: Not a barcode, but a patron's internal ID
#                       ou: Org unit where transaction happens
#                processor: Payment processor to use (AuthorizeNet, PayPal, etc)
#                       cc: credit card number
#                     cvv2: 3 or 4 digits from back of card
#                   amount: transaction value
#                   action: optional (default: Normal Authorization)
#               first_name: optional (default: patron's first_given_name field)
#                last_name: optional (default: patron's family_name field)
#                  address: optional (default: patron's street1 field + street2)
#                     city: optional (default: patron's city field)
#                    state: optional (default: patron's state field)
#                      zip: optional (default: patron's zip field)
#                  country: optional (some processor APIs: 2 letter code.)
#              description: optional
#                /, type => 'hash' }
#        ],
#        return => { desc => 'an ilsevent' }
#    }

sub process_payment {
    my ($argshash) = @_;

    # Confirm some required arguments.
    return OpenILS::Event->new('BAD_PARAMS')
        unless $argshash
            and $argshash->{cc}
            and $argshash->{amount}
            and $argshash->{expiration}
            and $argshash->{ou};

    if (!$argshash->{processor}) {
        if (!($argshash->{processor} =
                $U->ou_ancestor_setting_value(
                    $argshash->{ou}, CREDIT_NS . '.processor.default'))) {
            return OpenILS::Event->new('CREDIT_PROCESSOR_NOT_SPECIFIED');
        }
    }
    # Basic sanity check on processor name.
    if ($argshash->{processor} !~ /^[a-z0-9_\-]+$/i) {
        return OpenILS::Event->new('CREDIT_PROCESSOR_NOT_ALLOWED');
    }

    # Get org unit settings related to our processor
    my $psettings = get_processor_settings(
        $argshash->{ou}, $argshash->{processor}
    );

    if (!$psettings->{enabled}) {
        return OpenILS::Event->new('CREDIT_PROCESSOR_NOT_ENABLED');
    }

    # Add the org unit settings for the chosen processor to our argshash.
    $argshash = +{ %{$argshash}, %{$psettings} };

    # At least the following (derived from org unit settings) are required.
    return OpenILS::Event->new('CREDIT_PROCESSOR_BAD_PARAMS')
        unless $argshash->{login}
            and $argshash->{password};

    # A valid patron_id is also required.
    my $e = new_editor();
    my $patron = $e->retrieve_actor_user(
        [
            $argshash->{patron_id},
            {
                flesh        => 1,
                flesh_fields => { au => ["mailing_address"] }
            }
        ]
    ) or return $e->event;

    return dispatch($argshash, $patron);
}

sub prepare_bop_content {
    my ($argshash, $patron, $cardtype) = @_;

    my %content;
    foreach (qw/
        login
        password
        description
        first_name
        last_name
        amount
        expiration
        cvv2
        address
        city
        state
        zip
        country/) {
        if (exists $argshash->{$_}) {
            $content{$_} = $argshash->{$_};
        }
    }
    
    $content{action}       = $argshash->{action} || "Normal Authorization";
    $content{type}         = $cardtype;      #'American Express', 'VISA', 'MasterCard'
    $content{card_number}  = $argshash->{cc};
    $content{customer_id}  = $patron->id;
    
    $content{first_name} ||= $patron->first_given_name;
    $content{last_name}  ||= $patron->family_name;

    $content{FirstName}    = $content{first_name};   # kludge mcugly for PP
    $content{LastName}     = $content{last_name};


    # Especially for the following fields, do we need to support different
    # mapping of fields for different payment processors, particularly ones
    # in other countries?
    $content{address}    ||= $patron->mailing_address->street1;
    $content{address} .= ", " . $patron->mailing_address->street2
        if $patron->mailing_address->street2;

    $content{city}       ||= $patron->mailing_address->city;
    $content{state}      ||= $patron->mailing_address->state;
    $content{zip}        ||= $patron->mailing_address->post_code;
    $content{country}    ||= $patron->mailing_address->country;

    # Yet another fantastic kludge. country2code() comes from Locale::Country.
    # PayPal must have 2 letter country field (ISO 3166) that's uppercase.
    if (length($content{country}) > 2 && $argshash->{processor} eq 'PayPal') {
        $content{country} = uc country2code($content{country});
    }

    %content;
}

sub dispatch {
    my ($argshash, $patron) = @_;
    
    # The validate() sub is exported by Business::CreditCard.
    if (!validate($argshash->{cc})) {
        # Although it might help a troubleshooter, it's probably not a good
        # idea to put the credit card number in the log file.
        $logger->info("Credit card number invalid");

        return new OpenILS::Event("CREDIT_PROCESSOR_INVALID_CC_NUMBER");
    }

    # cardtype() also comes from Business::CreditCard.  It is not certain that
    # a) the card type returned by this method will be suitable input for
    #   a payment processor, nor that
    # b) it is even necessary to supply this argument to processors in all
    #   cases.  Testing this with several processors would be a good idea.
    (my $cardtype = cardtype($argshash->{cc})) =~ s/ card//i;

    if (lc($cardtype) eq "unknown") {
        $logger->info("Credit card number passed validate(), " .
            "yet cardtype() returned $cardtype");
        return new OpenILS::Event(
            "CREDIT_PROCESSOR_INVALID_CC_NUMBER", "note" => "cardtype $cardtype"
        );
    }

    $logger->debug(
        "applying payment via processor '" . $argshash->{processor} . "'"
    );

    # Find B:OP constructor arguments specific to our payment processor.
    my %bop_args = get_bop_args_filler($argshash);

    # We're assuming that all B:OP processors accept this argument to the
    # constructor.
    $bop_args{test_transaction} = $argshash->{testmode};

    my $transaction = new Business::OnlinePayment(
        $argshash->{processor}, %bop_args
    );

    $transaction->content(prepare_bop_content($argshash, $patron, $cardtype));

    # XXX submit() does not return a value, although crashing is possible here
    # with some bad input depending on the payment processor.
    $transaction->submit;

    my $payload = {
        "processor" => $argshash->{"processor"},
        "card_type" => $cardtype,
        "server_response" => $transaction->server_response
    };

    foreach (qw/authorization correlationid avs_code cvv2_code error_message/) {
        # authorization should always be present for successes, and
        # error_message should always be present for failures. The remaining
        # field may be important in PayPal transacations? Not sure.
        $payload->{$_} = $transaction->$_ if $transaction->can($_);
    }

    my $event_name;

    if ($transaction->is_success()) {
        $logger->info($argshash->{processor} . " payment succeeded");
        $event_name = "SUCCESS";
    } else {
        $logger->info($argshash->{processor} . " payment failed");
        $event_name = "CREDIT_PROCESSOR_DECLINED_TRANSACTION";
    }

    return new OpenILS::Event($event_name, "payload" => $payload);
}


1;
