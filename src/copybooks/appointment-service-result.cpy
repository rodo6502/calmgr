       01 APPOINTMENT-SERVICE-RESULT.
          05 ASO-STATUS-CODE         PIC 9(3).
          05 ASO-MESSAGE             PIC X(160).
          05 ASO-COUNT               PIC 9(5).
          05 ASO-APPOINTMENT.
             10 ASO-SCHEMA-VERSION   PIC 9(2).
             10 ASO-ID               PIC X(9).
             10 ASO-SERIES-ID        PIC X(9).
             10 ASO-START-DATE       PIC 9(8).
             10 ASO-END-DATE         PIC 9(8).
             10 ASO-ISO-YEAR         PIC 9(4).
             10 ASO-ISO-WEEK         PIC 9(2).
             10 ASO-CLIENT           PIC X(60).
             10 ASO-SUBJECT          PIC X(100).
             10 ASO-STATUS           PIC X.
             10 ASO-CREATED-AT       PIC X(20).
             10 ASO-UPDATED-AT       PIC X(20).
             10 ASO-CANCELLED-ON     PIC 9(8).
             10 ASO-CANCEL-NOTE      PIC X(120).
             10 ASO-RECURRENCE       PIC X.
             10 ASO-SERIES-UNTIL     PIC 9(8).
             10 ASO-REVISION         PIC 9(9).
          05 ASO-OUTPUT-PATH         PIC X(256).
          05 ASO-USER-ID             PIC X(12).
          05 ASO-USERNAME            PIC X(64).
          05 ASO-ROLE                PIC X(8).
          05 ASO-USER-ACTIVE         PIC X.
          05 ASO-USER-REVISION       PIC 9(9).
